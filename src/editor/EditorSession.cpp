#include "EditorSession.h"
#include "history/HistoryStore.h"
#include "image/ImageProcessor.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QtConcurrent>

namespace SnapLing {
EditorSession::EditorSession(QImage image)
    : ownedSettings(std::make_unique<Settings>()), ownedConfig(ownedSettings->provider()),
      settings(*ownedSettings), config(ownedConfig), document(std::move(image)) {
    initialize();
}
EditorSession::EditorSession(QImage image, Settings &s, ProviderConfig &c)
    : settings(s), config(c), document(std::move(image)) {
    initialize();
}
EditorSession::~EditorSession() { cancel(); }
void EditorSession::notify(QString message, bool error) {
    lastMessage = message;
    emit statusChanged(message, error);
}
void EditorSession::setTask(Task task) {
    currentTask = task;
    emit taskChanged();
}
void EditorSession::setTranslationEnabled(bool enabled) {
    if (translationEnabled == enabled)
        return;
    translationEnabled = enabled;
    emit translationStateChanged(enabled);
}
bool EditorSession::sourceUnchanged() {
    if (sourceKey == document.original().cacheKey())
        return true;
    translateAfterOcr = false;
    setTranslationEnabled(false);
    notify("图像已更换，请重新识别或翻译。", true);
    return false;
}
void EditorSession::initialize() {
    connect(&document, &Document::changed, this, [this] {
        if (currentTask == Task::Idle && !approvalPending)
            setTranslationEnabled(document.state().translationMode != 0 && !document.state().translations.isEmpty());
    });
#ifndef Q_OS_WIN
    connectOcr(paddleOcr, "paddleocr");
#endif
    connectOcr(tesseract, "tesseract");
    connect(&provider, &ITranslationProvider::completed, this, [this](auto units, qint64 ms) {
        setTask(Task::Idle);
        if (!translationEnabled)
            return;
        if (!sourceUnchanged())
            return;
        int overflow = 0;
        for (auto &unit : units) {
            unit.overflow = rendersTranslation(unit) &&
                            fitTranslation(unit.target, unit.box, document.original().size(), unit.fontSize).overflow;
            overflow += unit.overflow;
        }
        document.change([units](auto &state) {
            state.translations = units;
            state.translationMode = 1;
        });
        notify(QString("翻译完成，%1 ms。保留 %2 个原文区域（路径、同语言或识别存疑）。%3")
                   .arg(ms).arg(preservedLines)
                   .arg(overflow ? QString("%1 个译文区域放不下，保留原文；可在高级编辑器调整。").arg(overflow)
                                 : "可继续标注或进入高级编辑器。"));
        qInfo("translation_ms=%lld", ms);
    });
    connect(&provider, &ITranslationProvider::failed, this, [this](QString error) {
        setTask(Task::Idle);
        const bool enabled = translationEnabled;
        setTranslationEnabled(false);
        if (!enabled && error.contains("取消"))
            return;
        notify(error, true);
    });
}
void EditorSession::connectOcr(IOcrEngine &engine, const char *name) {
    connect(&engine, &IOcrEngine::progress, this, [this](int percent) {
        notify(QString("正在本地 OCR：%1% · 可继续标注或进入高级编辑器").arg(percent));
    });
    connect(&engine, &IOcrEngine::completed, this, [this, name](auto lines, qint64 ms, bool cold) {
        if (!sourceUnchanged()) {
            setTask(Task::Idle);
            return;
        }
        document.change([lines](auto &state) {
            state.ocr = lines;
            state.translations.clear();
            state.translationMode = 0;
        });
        setTask(Task::Idle);
        notify(QString("OCR 完成，%1 行，%2 ms").arg(lines.size()).arg(ms));
        qInfo("ocr_ms=%lld cold=%d lines=%lld engine=%s", ms, int(cold), qint64(lines.size()), name);
        if (translateAfterOcr) {
            translateAfterOcr = false;
            translationLines = lines;
            startTranslation();
        }
    });
    connect(&engine, &IOcrEngine::failed, this, [this](QString error) {
        const bool translating = translateAfterOcr;
        translateAfterOcr = false;
        setTask(Task::Idle);
        if (translating)
            setTranslationEnabled(false);
        if (!translationEnabled && error.contains("取消"))
            return;
        notify(error, true);
    });
}
void EditorSession::runOcr(bool thenTranslate) {
    if (currentTask != Task::Idle || approvalPending) {
        notify("识别或翻译任务正在运行，可继续编辑或取消当前任务。");
        return;
    }
    translateAfterOcr = thenTranslate;
    translateSelected = false;
    setTranslationEnabled(thenTranslate);
    sourceKey = document.original().cacheKey();
    setTask(Task::Ocr);
    notify("正在本地识别文字… 可继续编辑");
    IOcrEngine *engine = &tesseract;
#ifndef Q_OS_WIN
    engine = &paddleOcr;
    // Kept only for the native regression suite; installed builds always use PaddleOCR.
    if (qEnvironmentVariable("SNAPLING_OCR_TEST_ENGINE") == "tesseract")
        engine = &tesseract;
#endif
#ifndef Q_OS_WIN
    if (engine == &paddleOcr)
        engine->recognize(document.original(), RapidOcr::modelDirectory(), "chinese_english", true);
    else
#endif
        engine->recognize(document.original(), settings.value("ocr/path").toString(),
                          settings.value("ocr/languages", "chi_sim+eng").toString(),
                          settings.value("ocr/cache", true).toBool());
}
void EditorSession::translate(QVector<OcrLine> selected) {
    if (currentTask != Task::Idle || approvalPending) {
        notify("识别或翻译任务正在运行，可继续编辑或取消当前任务。");
        return;
    }
    setTranslationEnabled(true);
    if (document.state().ocr.isEmpty()) {
        runOcr(true);
        return;
    }
    sourceKey = document.original().cacheKey();
    translateSelected = !selected.isEmpty();
    translationLines = selected.isEmpty() ? document.state().ocr : std::move(selected);
    startTranslation();
}
void EditorSession::toggleTranslation(QVector<OcrLine> selected) {
    if (translationEnabled) {
        setTranslationEnabled(false);
        cancel();
        if (document.state().translationMode != 0)
            document.change([](auto &state) { state.translationMode = 0; });
        notify("已取消翻译，显示原图。标注和译文图层已保留。");
        return;
    }
    if (currentTask == Task::Idle && selected.isEmpty() && !document.state().translations.isEmpty() &&
        document.state().translations.first().language == config.targetLanguage) {
        document.change([](auto &state) { state.translationMode = 1; });
        notify("已显示译文，再次点击翻译可恢复原图。");
        return;
    }
    translate(std::move(selected));
}
void EditorSession::startTranslation() {
    if (translationLines.isEmpty()) {
        setTranslationEnabled(false);
        notify("没有可翻译的文字，请调整选区后重新 OCR。", true);
        return;
    }
    requestConfig = config;
    requestUnits = translationUnits(translationLines, requestConfig.targetLanguage, !translateSelected);
    preservedLines = translationLines.size() - requestUnits.size();
    if (requestUnits.isEmpty()) {
        setTranslationEnabled(false);
        notify("没有需要翻译的可靠文字。已保留路径、目标语言文字及识别存疑的内容，可在 OCR 模式检查或选中文字翻译。");
        return;
    }
    const auto endpoint = translationEndpoint();
    if (!requestConfig.isLocal()) {
        approvalPending = true;
        notify("等待允许将 OCR 文字发送到：" + endpoint);
        emit taskChanged();
        emit translationApprovalRequested(endpoint);
        return;
    }
    setTask(Task::Translating);
    notify("本机翻译：" + endpoint + " · 可继续编辑");
    provider.translate(requestUnits, requestConfig);
}
QString EditorSession::translationEndpoint() const {
    return QUrl(requestConfig.baseUrl)
        .adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
}
void EditorSession::approveTranslation(bool allowed) {
    if (!approvalPending)
        return;
    approvalPending = false;
    emit taskChanged();
    if (!allowed) {
        setTranslationEnabled(false);
        notify("已取消发送翻译内容。");
        return;
    }
    if (!sourceUnchanged())
        return;
    setTask(Task::Translating);
    notify("正在请求翻译服务… 可继续编辑");
    provider.translate(requestUnits, requestConfig);
}
void EditorSession::cancel() {
    if (currentTask != Task::Idle || approvalPending)
        setTranslationEnabled(false);
    translateAfterOcr = false;
    if (approvalPending)
        approveTranslation(false);
#ifndef Q_OS_WIN
    paddleOcr.cancel();
#endif
    tesseract.cancel();
    provider.cancel();
}
void EditorSession::copyImage() {
    QApplication::clipboard()->setImage(document.render());
    notify("已复制当前图片");
}
void EditorSession::requestSave(QWidget *parent) {
    const auto format = settings.value("general/format", "PNG").toString();
    const auto path = settings.value("general/saveDir", QDir::homePath() + "/Pictures").toString() +
                      "/SnapLing_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") +
                      (format == "JPG" ? ".jpg" : ".png");
    auto *dialog = new QFileDialog(parent, "保存图片", path, "PNG (*.png);;JPEG (*.jpg *.jpeg)");
    dialog->setObjectName("saveImageDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix(format == "JPG" ? "jpg" : "png");
    dialog->selectNameFilter(format == "JPG" ? "JPEG (*.jpg *.jpeg)" : "PNG (*.png)");
    connect(dialog, &QFileDialog::filterSelected, dialog, [dialog](QString filter) {
        dialog->setDefaultSuffix(filter.startsWith("JPEG") ? "jpg" : "png");
    });
    connect(dialog, &QFileDialog::fileSelected, this, &EditorSession::saveImage);
    dialog->open();
}
void EditorSession::saveImage(QString path) {
    if (path.isEmpty())
        return;
    const auto source = document.original();
    const auto state = document.state();
    const auto background = QColor(settings.value("general/jpegBackground", "#ffffff").toString());
    const int quality = settings.value("general/quality", 92).toInt();
    const bool keep = settings.value("history/enabled", false).toBool();
    const int days = settings.value("history/days", 30).toInt();
    const qint64 limit = settings.value("history/megabytes", 512).toLongLong() * 1024 * 1024;
    notify("正在保存图片… 可继续编辑");
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) {
            notify(error, true);
            return;
        }
        lastSavedPath = path;
        notify("已保存：" + path);
        emit imageSaved(path);
    });
    watcher->setFuture(QtConcurrent::run([source, state, path, background, quality, keep, days, limit] {
        Document snapshot(source);
        snapshot.change([state](auto &value) { value = state; });
        QString error;
        if (!snapshot.exportImage(path, error, background, quality))
            return error;
        if (keep) {
            HistoryStore history;
            history.add(snapshot.render(), !state.ocr.isEmpty(), !state.translations.isEmpty(), error);
            history.prune(days, limit);
        }
        return error;
    }));
}
} // namespace SnapLing

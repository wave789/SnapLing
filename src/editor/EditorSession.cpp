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
bool EditorSession::sourceUnchanged() {
    if (sourceKey == document.original().cacheKey())
        return true;
    translateAfterOcr = false;
    notify("图像已更换，请重新识别或翻译。", true);
    return false;
}
void EditorSession::initialize() {
    connect(&ocr, &IOcrEngine::progress, this, [this](int percent) {
        notify(QString("正在本地 OCR：%1% · 可继续标注或进入高级编辑器").arg(percent));
    });
    connect(&ocr, &IOcrEngine::completed, this, [this](auto lines, qint64 ms, bool cold) {
        setTask(Task::Idle);
        if (!sourceUnchanged())
            return;
        document.change([lines](auto &state) {
            state.ocr = lines;
            state.translations.clear();
            state.translationMode = 0;
        });
        notify(QString("OCR 完成，%1 行，%2 ms").arg(lines.size()).arg(ms));
        qInfo("ocr_ms=%lld cold=%d lines=%lld", ms, int(cold), qint64(lines.size()));
        if (translateAfterOcr) {
            translateAfterOcr = false;
            translationLines = lines;
            startTranslation();
        }
    });
    connect(&ocr, &IOcrEngine::failed, this, [this](QString error) {
        translateAfterOcr = false;
        setTask(Task::Idle);
        notify(error, true);
    });
    connect(&provider, &ITranslationProvider::completed, this, [this](auto units, qint64 ms) {
        setTask(Task::Idle);
        if (!sourceUnchanged())
            return;
        int overflow = 0;
        for (auto &unit : units) {
            unit.overflow = fitTranslation(unit.target, unit.box, document.original().size(), unit.fontSize).overflow;
            overflow += unit.overflow;
        }
        document.change([units](auto &state) {
            state.translations = units;
            state.translationMode = 1;
        });
        notify(QString("翻译完成，%1 ms，%2 个区域需调整。高级编辑器可精细调整译文。")
                   .arg(ms).arg(overflow));
        qInfo("translation_ms=%lld", ms);
    });
    connect(&provider, &ITranslationProvider::failed, this, [this](QString error) {
        setTask(Task::Idle);
        notify(error, true);
    });
}
void EditorSession::runOcr(bool thenTranslate) {
    if (currentTask != Task::Idle || approvalPending) {
        notify("识别或翻译任务正在运行，可继续编辑或取消当前任务。");
        return;
    }
    translateAfterOcr = thenTranslate;
    sourceKey = document.original().cacheKey();
    setTask(Task::Ocr);
    notify("正在本地识别文字… 可继续编辑");
    ocr.recognize(document.original(), settings.value("ocr/path", findTessdata()).toString(),
                  settings.value("ocr/languages", "chi_sim+eng").toString(),
                  settings.value("ocr/cache", true).toBool());
}
void EditorSession::translate(QVector<OcrLine> selected) {
    if (currentTask != Task::Idle || approvalPending) {
        notify("识别或翻译任务正在运行，可继续编辑或取消当前任务。");
        return;
    }
    if (document.state().ocr.isEmpty()) {
        runOcr(true);
        return;
    }
    sourceKey = document.original().cacheKey();
    translationLines = selected.isEmpty() ? document.state().ocr : std::move(selected);
    startTranslation();
}
void EditorSession::startTranslation() {
    if (translationLines.isEmpty()) {
        notify("没有可翻译的文字，请调整选区后重新 OCR。", true);
        return;
    }
    requestConfig = config;
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
    provider.translate(translationUnits(translationLines, requestConfig.targetLanguage), requestConfig);
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
        notify("已取消发送翻译内容。");
        return;
    }
    if (!sourceUnchanged())
        return;
    setTask(Task::Translating);
    notify("正在请求翻译服务… 可继续编辑");
    provider.translate(translationUnits(translationLines, requestConfig.targetLanguage), requestConfig);
}
void EditorSession::cancel() {
    translateAfterOcr = false;
    if (approvalPending)
        approveTranslation(false);
    ocr.cancel();
    provider.cancel();
}
void EditorSession::copyImage() {
    QApplication::clipboard()->setImage(document.render());
    notify("已复制当前图片");
}
void EditorSession::copyTranslation() {
    QStringList text;
    for (const auto &unit : document.state().translations)
        if (unit.status == "done")
            text << unit.target;
    QApplication::clipboard()->setText(text.join('\n'));
    notify("已复制译文");
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

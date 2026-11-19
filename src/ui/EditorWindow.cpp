#include "EditorWindow.h"
#include "PinWindow.h"
#include "history/HistoryStore.h"
#include "image/ImageProcessor.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QtConcurrent>
namespace SnapLing {
EditorWindow::EditorWindow(QImage image, Settings &s, ProviderConfig &c)
    : settings(s), config(c), doc(std::move(image)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("SnapLing · 截图编辑");
    resize(1180, 780);
    auto *scroll = new QScrollArea;
    canvas = new Canvas(&doc);
    scroll->setWidget(canvas);
    scroll->setWidgetResizable(true);
    setCentralWidget(scroll);
    auto *tools = addToolBar("标注工具");
    tools->setMovable(false);
    auto *group = new QActionGroup(this);
    group->setExclusive(true);
    const QList<QPair<QString, Tool>> list{
        {"选择", Tool::Select}, {"矩形", Tool::Rectangle}, {"椭圆", Tool::Ellipse},     {"直线", Tool::Line},
        {"箭头", Tool::Arrow},  {"画笔", Tool::Pen},       {"荧光", Tool::Highlighter}, {"文字", Tool::Text},
        {"序号", Tool::Number}, {"马赛克", Tool::Mosaic},  {"模糊", Tool::Blur},        {"裁剪", Tool::Crop},
        {"取色", Tool::Picker}};
    for (auto [name, tool] : list) {
        auto *a = tools->addAction(name);
        a->setCheckable(true);
        group->addAction(a);
        if (tool == Tool::Select)
            a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, tool] { canvas->setTool(tool); });
    }
    tools->addSeparator();
    auto *color = tools->addAction("颜色");
    connect(color, &QAction::triggered, this, [this] {
        auto c = QColorDialog::getColor(canvas->color(), this);
        if (c.isValid())
            canvas->setColor(c);
    });
    auto *width = new QSpinBox;
    width->setRange(1, 50);
    width->setValue(3);
    width->setPrefix("线宽 ");
    tools->addWidget(width);
    connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas, &Canvas::setWidth);
    auto *font = tools->addAction("字体");
    connect(font, &QAction::triggered, this, [this] {
        bool ok;
        auto f = QFontDialog::getFont(&ok, QFont(), this);
        if (ok) {
            if (f.pixelSize() < 1)
                f.setPixelSize(qRound(f.pointSizeF() * 96 / 72));
            canvas->setFont(f);
        }
    });
    auto *opacity = new QDoubleSpinBox;
    opacity->setRange(.05, 1);
    opacity->setSingleStep(.1);
    opacity->setValue(1);
    opacity->setPrefix("透明度 ");
    tools->addWidget(opacity);
    connect(opacity, qOverload<double>(&QDoubleSpinBox::valueChanged), canvas, &Canvas::setOpacity);
    addToolBarBreak();
    auto *actions = addToolBar("截图操作");
    actions->setMovable(false);
    auto action = [&](QString name, auto fn, QKeySequence shortcut = {}) {
        auto *a = actions->addAction(name);
        connect(a, &QAction::triggered, this, fn);
        if (!shortcut.isEmpty())
            a->setShortcut(shortcut);
        return a;
    };
    auto *undo = action(
             "撤销", [this] { doc.undo(); }, QKeySequence::Undo),
         *redo = action("重做", [this] { doc.redo(); }, QKeySequence("Ctrl+Shift+Z"));
    connect(&doc, &Document::changed, this, [this, undo, redo] {
        undo->setEnabled(doc.canUndo());
        redo->setEnabled(doc.canRedo());
    });
    undo->setEnabled(false);
    redo->setEnabled(false);
    action("旋转", [this] { doc.change([](auto &s) { s.rotation = (s.rotation + 90) % 360; }); });
    action("恢复裁剪", [this] { doc.change([this](auto &s) { s.crop = doc.original().rect(); }); });
    action("水平翻转", [this] { doc.change([](auto &s) { s.flipH = !s.flipH; }); });
    action("垂直翻转", [this] { doc.change([](auto &s) { s.flipV = !s.flipV; }); });
    actions->addSeparator();
    action("OCR", [this] { runOcr(); }, QKeySequence("Ctrl+Shift+O"));
    action("选全部文字", [this] { canvas->selectAllText(); });
    action("复制全部文字", [this] {
        TextLayout l;
        l.setLines(doc.state().ocr);
        QApplication::clipboard()->setText(l.all());
    });
    action("导出文字", [this] { exportText(); });
    action("原地翻译", [this] { translate(); });
    action("调整译文", [this] { editTranslation(); });
    auto *mode = new QComboBox;
    mode->addItems({"原文", "译文", "对照"});
    actions->addWidget(mode);
    comparison = new QDockWidget("原始截图", this);
    auto *original = new QLabel;
    original->setAlignment(Qt::AlignCenter);
    original->setPixmap(
        QPixmap::fromImage(doc.original().scaled(560, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    comparison->setWidget(original);
    addDockWidget(Qt::RightDockWidgetArea, comparison);
    comparison->hide();
    connect(&doc, &Document::changed, this, [this, original, mode] {
        mode->setCurrentIndex(doc.state().translationMode);
        comparison->setVisible(doc.state().translationMode == 2);
        auto key = doc.original().cacheKey();
        if (original->property("sourceKey").toLongLong() != key) {
            original->setPixmap(QPixmap::fromImage(
                doc.original().scaled(560, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            original->setProperty("sourceKey", key);
        }
    });
    connect(mode, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        doc.change([i](auto &s) { s.translationMode = i; });
        comparison->setVisible(i == 2);
    });
    auto *panel = new QComboBox;
    panel->addItems({"自动背景", "纯色底板"});
    actions->addWidget(panel);
    connect(panel, qOverload<int>(&QComboBox::activated), this,
            [this](int i) { doc.change([i](auto &s) { s.panel = i == 1; }); });
    cancelAction = action("取消任务", [this] {
        ocr.cancel();
        provider.cancel();
    });
    cancelAction->setEnabled(false);
    addToolBarBreak();
    auto *output = addToolBar("输出");
    output->setMovable(false);
    auto outputAction = [&](QString name, auto fn, QKeySequence shortcut = {}) {
        auto *a = output->addAction(name);
        connect(a, &QAction::triggered, this, fn);
        a->setShortcut(shortcut);
        return a;
    };
    outputAction("复制", [this] { copy(); }, QKeySequence::Copy);
    outputAction("保存图片", [this] { saveImage(); }, QKeySequence::Save);
    outputAction("保存工程", [this] { saveProject(); }, QKeySequence("Ctrl+Shift+S"));
    outputAction("复制文件路径", [this] {
        if (savedPath.isEmpty())
            saveImage();
        if (!savedPath.isEmpty())
            QApplication::clipboard()->setText(savedPath);
    });
    outputAction("复制译文", [this] {
        QStringList texts;
        for (auto u : doc.state().translations)
            if (u.status == "done")
                texts << u.target;
        QApplication::clipboard()->setText(texts.join('\n'));
    });
    outputAction("贴图", [this] { new PinWindow(doc.render()); });
    outputAction("二维码", [this] {
        QString error;
        auto result = decodeQr(doc.render(), error);
        QMessageBox::information(this, "二维码",
                                 error.isEmpty() ? (result.isEmpty() ? "未识别到二维码" : result.join("\n\n"))
                                                 : error);
        if (!result.isEmpty())
            QApplication::clipboard()->setText(result.join('\n'));
    });
    outputAction("隐私打码建议", [this] { sensitive(); });
    outputAction("搜索选中文字", [this] {
        auto text = canvas->selectedText();
        if (!text.isEmpty())
            QDesktopServices::openUrl(QUrl("https://www.google.com/search?q=" +
                                           QString::fromLatin1(QUrl::toPercentEncoding(text))));
    });
    outputAction("设置", [this] { emit settingsRequested(); });
    outputAction("关闭", [this] { close(); }, QKeySequence(Qt::Key_Escape));
    connect(canvas, &Canvas::copyRequested, this, &EditorWindow::copy);
    connect(canvas, &Canvas::status, this, [this](QString m) { statusBar()->showMessage(m, 8000); });
    connect(&ocr, &IOcrEngine::completed, this, [this, mode](auto lines, qint64 ms, bool cold) {
        doc.change([lines](auto &s) {
            s.ocr = lines;
            s.translations.clear();
            s.translationMode = 0;
        });
        mode->setCurrentIndex(0);
        canvas->setTool(Tool::Ocr);
        setBusy(false, QString("OCR 完成：%1 行，%2 ms（%3），字符位置为估算值")
                           .arg(lines.size())
                           .arg(ms)
                           .arg(cold ? "冷启动" : "热启动"));
        qInfo("ocr_ms=%lld cold=%d lines=%lld", ms, int(cold), qint64(lines.size()));
        if (afterOcrTranslate) {
            afterOcrTranslate = false;
            startTranslation();
        }
    });
    connect(&ocr, &IOcrEngine::progress, this, [this](int p) {
        if (busy)
            statusBar()->showMessage(QString("正在本地 OCR：%1% · 可取消").arg(p));
    });
    connect(&ocr, &IOcrEngine::failed, this, [this](QString e) {
        afterOcrTranslate = false;
        setBusy(false, e);
        QMessageBox::warning(this, "OCR", e);
    });
    connect(&provider, &ITranslationProvider::completed, this, [this, mode](auto units, qint64 ms) {
        int overflow = 0;
        for (auto &u : units) {
            auto fit = fitTranslation(u.target, u.box, doc.original().size(), u.fontSize);
            u.overflow = fit.overflow;
            if (fit.overflow)
                ++overflow;
        }
        doc.change([units](auto &s) {
            s.translations = units;
            s.translationMode = 1;
        });
        mode->setCurrentIndex(1);
        setBusy(false,
                QString("翻译完成，%1 ms。%2 个区域需手工调整；可点击“调整译文”。").arg(ms).arg(overflow));
        qInfo("translation_ms=%lld", ms);
    });
    connect(&provider, &ITranslationProvider::failed, this, [this](QString e) {
        setBusy(false, e);
        QMessageBox::warning(this, "翻译", e);
    });
    statusBar()->showMessage(
        "双击文字可修改；选择工具可移动/删除标注；滚轮缩放。OCR 中 Ctrl+C 优先复制所选文字。");
}
void EditorWindow::setBusy(bool b, QString m) {
    busy = b;
    cancelAction->setEnabled(b);
    statusBar()->showMessage(m);
}
void EditorWindow::runOcr(bool then) {
    if (busy)
        return;
    afterOcrTranslate = then;
    setBusy(true, "正在本地识别文字… 可取消");
    ocr.recognize(doc.original(), settings.value("ocr/path", findTessdata()).toString(),
                  settings.value("ocr/languages", "chi_sim+eng").toString(),
                  settings.value("ocr/cache", true).toBool());
}
void EditorWindow::translate() {
    if (busy)
        return;
    if (doc.state().ocr.isEmpty())
        runOcr(true);
    else
        startTranslation();
}
void EditorWindow::startTranslation() {
    QString endpoint = QUrl(config.baseUrl)
                           .adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment)
                           .toString();
    QString message = config.isLocal() ? "将 OCR 文字发送到本机服务：\n" + endpoint
                                       : "将 OCR 文字发送到远程服务：\n" + endpoint +
                                             "\n请确认你允许向该服务发送所选内容。";
    if (!config.isLocal() &&
        QMessageBox::question(this, "翻译内容发送确认", message, QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    auto lines = doc.state().ocr;
    auto selected = canvas->selectedLines();
    if (!selected.isEmpty())
        lines = selected;
    setBusy(true, (config.isLocal() ? "本机翻译：" : "远程翻译：") + endpoint + " · 可取消");
    provider.translate(translationUnits(lines, config.targetLanguage), config);
}
void EditorWindow::copy() {
    if (!canvas->selectedText().isEmpty())
        QApplication::clipboard()->setText(canvas->selectedText());
    else
        QApplication::clipboard()->setImage(doc.render());
    statusBar()->showMessage("已复制", 3000);
    if (settings.value("general/closeAfterCopy", false).toBool())
        close();
}
void EditorWindow::saveImage() {
    auto format = settings.value("general/format", "PNG").toString();
    auto path = QFileDialog::getSaveFileName(
        this, "保存图片",
        settings.value("general/saveDir", QDir::homePath() + "/Pictures").toString() + "/SnapLing_" +
            QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + (format == "JPG" ? ".jpg" : ".png"),
        "PNG (*.png);;JPEG (*.jpg *.jpeg)");
    if (path.isEmpty())
        return;
    // 冻结编辑状态，在工作线程渲染、编码和写入，避免保存 4K 图像阻塞界面。
    auto source = doc.original();
    auto state = doc.state();
    auto bg = QColor(settings.value("general/jpegBackground", "#ffffff").toString());
    int quality = settings.value("general/quality", 92).toInt();
    bool keep = settings.value("history/enabled", false).toBool();
    int days = settings.value("history/days", 30).toInt();
    qint64 limit = settings.value("history/megabytes", 512).toLongLong() * 1024 * 1024;
    auto *w = new QFutureWatcher<QString>(this);
    connect(w, &QFutureWatcher<QString>::finished, this, [this, w, path] {
        auto error = w->result();
        w->deleteLater();
        if (!error.isEmpty())
            QMessageBox::warning(this, "保存失败", error);
        else {
            savedPath = path;
            statusBar()->showMessage("已保存：" + path, 8000);
        }
    });
    w->setFuture(QtConcurrent::run([source, state, path, bg, quality, keep, days, limit] {
        Document d(source);
        d.change([state](auto &s) { s = state; });
        QString error;
        if (!d.exportImage(path, error, bg, quality))
            return error;
        if (keep) {
            HistoryStore h;
            h.add(d.render(), !state.ocr.isEmpty(), !state.translations.isEmpty(), error);
            h.prune(days, limit);
        }
        return error;
    }));
}
void EditorWindow::saveProject() {
    auto path = QFileDialog::getSaveFileName(this, "保存可编辑工程", {}, "SnapLing (*.snapling)");
    if (path.isEmpty())
        return;
    auto source = doc.original();
    auto state = doc.state();
    auto *w = new QFutureWatcher<QString>(this);
    connect(w, &QFutureWatcher<QString>::finished, this, [this, w] {
        auto error = w->result();
        w->deleteLater();
        if (!error.isEmpty())
            QMessageBox::warning(this, "保存工程失败", error);
        else
            statusBar()->showMessage("工程已保存", 4000);
    });
    w->setFuture(QtConcurrent::run([source, state, path] {
        Document d(source);
        d.change([state](auto &s) { s = state; });
        QString error;
        d.saveProject(path, error);
        return error;
    }));
}
bool EditorWindow::loadProject(QString path) {
    QString error;
    if (!doc.loadProject(path, error)) {
        QMessageBox::warning(this, "打开工程失败", error);
        return false;
    }
    return true;
}
void EditorWindow::exportText() {
    TextLayout text;
    text.setLines(doc.state().ocr);
    if (text.all().isEmpty())
        return;
    auto path = QFileDialog::getSaveFileName(this, "导出 OCR 文字", {}, "TXT (*.txt);;Markdown (*.md)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    auto bytes = text.all().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, "导出失败", "无法保存文字文件");
}
void EditorWindow::sensitive() {
    if (doc.state().ocr.isEmpty()) {
        runOcr();
        return;
    }
    QRegularExpression regex(
        "(?:1[3-9][0-9]{9}|[\\w.+-]+@[\\w.-]+\\.[A-Za-z]{2,}|(?:[0-9]{1,3}\\.){3}[0-9]{1,3})");
    QVector<QRectF> boxes;
    for (auto line : doc.state().ocr)
        if (regex.match(line.text).hasMatch())
            boxes << line.box;
    if (boxes.isEmpty()) {
        QMessageBox::information(this, "隐私建议", "未发现手机号、邮箱或 IP。你仍可手工打码。");
        return;
    }
    if (QMessageBox::question(this, "确认打码",
                              QString("发现 %1 行可能包含敏感信息，是否对这些行打码？").arg(boxes.size())) !=
        QMessageBox::Yes)
        return;
    doc.change([boxes](auto &s) {
        for (auto b : boxes) {
            Annotation a;
            a.tool = Tool::Mosaic;
            a.width = 6;
            a.points << b.topLeft() << b.bottomRight();
            s.annotations << a;
        }
    });
}
void EditorWindow::editTranslation() {
    auto units = doc.state().translations;
    if (units.isEmpty())
        return;
    QStringList choices;
    for (auto &u : units)
        choices << u.id + " · " + u.target.left(40);
    bool ok;
    auto choice = QInputDialog::getItem(this, "调整译文", "选择文字块", choices, 0, false, &ok);
    if (!ok)
        return;
    int index = choices.indexOf(choice);
    auto text = QInputDialog::getMultiLineText(this, "修改译文", "完整译文", units[index].target, &ok);
    if (!ok)
        return;
    units[index].target = text;
    int size = QInputDialog::getInt(this, "译文字号", "0 表示自动；无法容纳时可扩展区域",
                                    units[index].fontSize, 0, 96, 1, &ok);
    if (!ok)
        return;
    units[index].fontSize = size;
    int height = QInputDialog::getInt(this, "译文区域", "区域高度（像素）", qRound(units[index].box.height()),
                                      8, doc.original().height(), 1, &ok);
    if (ok)
        units[index].box.setHeight(height);
    doc.change([units](auto &s) { s.translations = units; });
}
} // namespace SnapLing

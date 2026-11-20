#include "EditorWindow.h"
#include "editor/AnnotationTools.h"
#include "PinWindow.h"
#include "image/ImageProcessor.h"
#include "ToolIcons.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScreen>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QStyle>
#include <QtConcurrent>
namespace SnapLing {
EditorWindow::EditorWindow(QImage image, Settings &s, ProviderConfig &c)
    : EditorWindow(std::make_shared<EditorSession>(std::move(image), s, c)) {}
EditorWindow::EditorWindow(std::shared_ptr<EditorSession> session)
    : editing(std::move(session)), settings(editing->settings),
      doc(editing->document) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("SnapLing · 截图编辑");
    const auto available = QGuiApplication::primaryScreen()->availableGeometry().size() - QSize(32, 32);
    resize(QSize(1180, 780).boundedTo(available));
    auto *scroll = new QScrollArea;
    canvas = new Canvas(&doc);
    scroll->setWidget(canvas);
    scroll->setWidgetResizable(true);
    setCentralWidget(scroll);
    auto *tools = addToolBar("标注工具");
    tools->setMovable(false);
    addAnnotationTools(tools, canvas, true);
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
    undo->setEnabled(doc.canUndo());
    redo->setEnabled(doc.canRedo());
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
    auto *translation = action("原地翻译", [this] { translate(); });
    translation->setIcon(toolIcon(ToolIcon::Translate));
    translation->setCheckable(true);
    auto *translationButton = qobject_cast<QToolButton *>(actions->widgetForAction(translation));
    translationButton->setObjectName("translationButton");
    translationButton->setAccessibleName("原地翻译");
    actions->setStyleSheet("QToolButton[translationActive=\"true\"]{background:#08b783;border-radius:5px}");
    auto updateTranslation = [translation, translationButton](bool enabled) {
        translation->setChecked(enabled);
        translation->setToolTip(enabled ? "取消翻译，恢复原图" : "原地翻译");
        translationButton->setToolTip(translation->toolTip());
        translationButton->setProperty("translationActive", enabled);
        translationButton->style()->unpolish(translationButton);
        translationButton->style()->polish(translationButton);
        translationButton->update();
    };
    connect(editing.get(), &EditorSession::translationStateChanged, this, updateTranslation);
    connect(translation, &QAction::triggered, this, [this, updateTranslation] {
        updateTranslation(editing->isTranslationEnabled());
    });
    updateTranslation(editing->isTranslationEnabled());
    action("调整译文", [this] { editTranslation(); });
    auto *mode = new QComboBox;
    mode->addItems({"原文", "译文", "对照"});
    mode->setCurrentIndex(doc.state().translationMode);
    actions->addWidget(mode);
    comparison = new QDockWidget("原始截图", this);
    auto *original = new QLabel;
    original->setAlignment(Qt::AlignCenter);
    original->setPixmap(
        QPixmap::fromImage(doc.original().scaled(560, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    comparison->setWidget(original);
    addDockWidget(Qt::RightDockWidgetArea, comparison);
    comparison->setVisible(doc.state().translationMode == 2);
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
        editing->cancel();
    });
    cancelAction->setEnabled(editing->task() != EditorSession::Task::Idle || editing->awaitingApproval());
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
    connect(editing.get(), &EditorSession::statusChanged, this, [this](QString message, bool) {
        statusBar()->showMessage(message);
    });
    connect(editing.get(), &EditorSession::taskChanged, this, [this] {
        cancelAction->setEnabled(editing->task() != EditorSession::Task::Idle || editing->awaitingApproval());
    });
    connect(editing.get(), &EditorSession::imageSaved, this, [this](QString path) { savedPath = path; });
    connect(editing.get(), &EditorSession::translationApprovalRequested,
            this, &EditorWindow::requestTranslationApproval);
    if (editing->awaitingApproval())
        QTimer::singleShot(0, this, [this] { requestTranslationApproval(editing->translationEndpoint()); });
    savedPath = editing->savedPath();
    statusBar()->showMessage(editing->message().isEmpty()
        ? "双击文字可修改；选择工具可移动/删除标注；滚轮缩放。OCR 中 Ctrl+C 优先复制所选文字。"
        : editing->message());
}
EditorWindow::~EditorWindow() {
    disconnect(editing.get(), nullptr, this, nullptr);
    disconnect(&doc, nullptr, this, nullptr);
    canvas->finishTextEdit(true);
    delete canvas;
    canvas = nullptr;
}
void EditorWindow::requestTranslationApproval(QString endpoint) {
    auto *dialog = new QMessageBox(QMessageBox::Question, "翻译内容发送确认",
        "允许将 OCR 文字发送到该服务？\n" + QUrl(endpoint).adjusted(
            QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString(),
        QMessageBox::Yes | QMessageBox::No, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    connect(dialog, &QMessageBox::finished, editing.get(), [session = editing](int result) {
        session->approveTranslation(result == QMessageBox::Yes);
    });
    dialog->show();
}
void EditorWindow::runOcr(bool thenTranslate) {
    canvas->setTool(Tool::Ocr);
    editing->runOcr(thenTranslate);
}
void EditorWindow::translate() {
    editing->toggleTranslation(canvas->selectedLines());
}
void EditorWindow::copy() {
    if (!canvas->selectedText().isEmpty())
        QApplication::clipboard()->setText(canvas->selectedText());
    else
        editing->copyImage();
    statusBar()->showMessage("已复制", 3000);
    if (settings.value("general/closeAfterCopy", false).toBool())
        close();
}
void EditorWindow::saveImage() {
    canvas->finishTextEdit(true);
    editing->requestSave(this);
}
void EditorWindow::saveProject() {
    canvas->finishTextEdit(true);
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

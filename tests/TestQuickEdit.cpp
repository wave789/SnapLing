#include "app/Application.h"
#include "selection/SelectionOverlay.h"
#include "ui/EditorWindow.h"
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QMessageBox>
#include <QProcess>
#include <QSignalSpy>
#include <QScreen>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QToolButton>
#include <QtTest>
#include <X11/Xlib.h>

using namespace SnapLing;
class TestQuickEdit : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir files;
    QTcpServer server;
    QProcess fixture;
    std::unique_ptr<Application> application;
    QPointer<SelectionOverlay> overlay;
    QString targetWindow;
    int delay = 0, responseStatus = 200, requests = 0;

    template <class T> static QList<T *> windows() {
        QList<T *> result;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<T *>(widget))
                result << window;
        return result;
    }
    QString control(QStringList arguments) {
        QProcess process;
        process.start("xdotool", arguments);
        if (!process.waitForFinished(5000) || process.exitCode() != 0)
            return {};
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    void click(QString text) {
        QVERIFY(overlay);
        auto *toolbar = overlay->findChild<QFrame *>("quickToolbar");
        QVERIFY(toolbar && toolbar->isVisible());
        for (auto *button : toolbar->findChildren<QToolButton *>())
            if (button->defaultAction() && button->defaultAction()->text() == text) {
                QVERIFY2(button->isVisible(), qPrintable("工具被隐藏：" + text));
                QTest::mouseClick(button, Qt::LeftButton);
                return;
            }
        QFAIL(qPrintable("未找到工具：" + text));
    }
    void capture(QRect region = QRect(15, 15, 765, 475)) {
        control({"windowfocus", targetWindow});
        control({"key", "Escape"});
        QTest::qWait(80);
        control({"key", "ctrl+alt+a"});
        QTRY_VERIFY_WITH_TIMEOUT(!windows<SelectionOverlay>().isEmpty(), 5000);
        overlay = windows<SelectionOverlay>().first();
        QVERIFY(windows<EditorWindow>().isEmpty());
        const qreal dpr = qApp->primaryScreen()->devicePixelRatio();
        // 外部测试窗口位于逻辑坐标 (40, 40)，鼠标事件与捕获像素各自按 DPR 映射。
        const auto geometry = control({"getwindowgeometry", "--shell", targetWindow});
        QRegularExpression x("(?:^|\n)X=(\\d+)"), y("(?:^|\n)Y=(\\d+)");
        const QPoint origin(qRound(x.match(geometry).captured(1).toInt() / dpr),
                            qRound(y.match(geometry).captured(1).toInt() / dpr));
        const auto from = overlay->mapFromGlobal(origin + region.topLeft());
        const auto to = overlay->mapFromGlobal(origin + region.topLeft() + QPoint(region.width(), region.height()));
        QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(overlay, to, 20);
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
        QVERIFY(overlay->canvasWidget()->isVisible());
        QVERIFY(windows<EditorWindow>().isEmpty());
    }
    void draw(QString tool, QPointF from = {80, 70}, QPointF to = {260, 140}) {
        click(tool);
        auto *canvas = overlay->canvasWidget();
        const auto transform = canvas->sourceToWidget();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(from).toPoint());
        QTest::mouseMove(canvas, transform.map(to).toPoint(), 20);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(to).toPoint());
    }
    void waitForTranslation() {
        QTRY_VERIFY_WITH_TIMEOUT(!overlay->session()->document.state().translations.isEmpty(), 10000);
        QCOMPARE(overlay->session()->document.state().translationMode, 1);
        QVERIFY(windows<EditorWindow>().isEmpty());
    }
    void serve(QTcpSocket *socket) {
        auto bytes = socket->property("request").toByteArray() + socket->readAll();
        socket->setProperty("request", bytes);
        const auto boundary = bytes.indexOf("\r\n\r\n");
        if (boundary < 0 || socket->property("responded").toBool())
            return;
        QRegularExpression length("Content-Length: ([0-9]+)", QRegularExpression::CaseInsensitiveOption);
        const auto match = length.match(QString::fromLatin1(bytes.left(boundary)));
        if (!match.hasMatch() || bytes.size() < boundary + 4 + match.captured(1).toInt())
            return;
        socket->setProperty("responded", true);
        ++requests;
        const auto payload = QJsonDocument::fromJson(bytes.mid(boundary + 4)).object();
        const auto messages = payload["messages"].toArray();
        const auto data = QJsonDocument::fromJson(messages.last().toObject()["content"].toString().toUtf8()).object();
        QJsonArray translations;
        for (const auto &block : data["blocks"].toArray())
            translations.append(QJsonObject{{"id", block.toObject()["id"]}, {"text", "你好世界"}});
        const auto content = QString::fromUtf8(QJsonDocument(QJsonObject{{"translations", translations}})
                                                  .toJson(QJsonDocument::Compact));
        const auto response = QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{
            {"message", QJsonObject{{"content", content}}}, {"finish_reason", "stop"}}}}})
                                  .toJson(QJsonDocument::Compact);
        QTimer::singleShot(delay, socket, [socket, response, status = responseStatus] {
            socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
            socket->disconnectFromHost();
        });
    }

  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    void init() {
        delay = requests = 0;
        responseStatus = 200;
        Settings settings;
        settings.set("hotkey/capture", "Ctrl+Alt+A");
        settings.set("ocr/path", findTessdata());
        settings.set("ocr/languages", "chi_sim+eng");
        ProviderConfig config;
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        settings.saveProvider(config);
        fixture.start(QCoreApplication::applicationDirPath() + "/snapling_desktop_test", {"--target-only"});
        QVERIFY(fixture.waitForStarted(3000));
        targetWindow = control({"search", "--sync", "--name", "^SnapLing real desktop target$"}).split('\n').first();
        QVERIFY(!targetWindow.isEmpty());
        application = std::make_unique<Application>();
        QTest::qWait(100);
    }
    void cleanup() {
        application.reset();
        overlay = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (auto *socket : server.findChildren<QTcpSocket *>())
            socket->abort();
        fixture.terminate();
        QVERIFY(fixture.waitForFinished(3000));
    }
    void instantAnnotations() {
        capture();
        QVERIFY(overlay);
        auto &document = overlay->session()->document;
        const auto original = document.original();
        draw("箭头");
        draw("矩形", {300, 80}, {500, 200});
        QCOMPARE(document.state().annotations.size(), 2);
        QCOMPARE(document.original(), original);
        QVERIFY(document.render() != original);
        QVERIFY(windows<EditorWindow>().isEmpty());
        auto *toolbar = overlay->findChild<QFrame *>("quickToolbar");
        QVERIFY(overlay->rect().contains(toolbar->geometry()));
        const int scale = qRound(overlay->devicePixelRatioF() * 100);
        const auto suffix = scale == 100 ? QString() : "-" + QString::number(scale);
        QVERIFY(overlay->grab().save(QCoreApplication::applicationDirPath() + "/quick-edit-ui" + suffix + ".png"));
        click("撤销");
        QCOMPARE(document.state().annotations.size(), 1);
        click("重做");
        QCOMPARE(document.state().annotations.size(), 2);
    }
    void instantOcr() {
        capture({20, 250, 760, 150});
        QVERIFY(overlay);
        click("OCR");
        QTRY_VERIFY_WITH_TIMEOUT(!overlay->session()->document.state().ocr.isEmpty(), 10000);
        TextLayout text;
        text.setLines(overlay->session()->document.state().ocr);
        QVERIFY2(text.all().contains("SnapLing"), qPrintable(text.all()));
        QVERIFY2(text.all().contains("中文"), qPrintable(text.all()));
        const auto boxes = text.characterBoxes(0);
        auto *canvas = overlay->canvasWidget();
        const auto transform = canvas->sourceToWidget();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(boxes.first().center()).toPoint());
        QTest::mouseMove(canvas, transform.map(boxes.last().center()).toPoint(), 20);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(boxes.last().center()).toPoint());
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), text.lines().first().text);
        const auto last = text.characterBoxes(text.lines().size() - 1);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(boxes.first().center()).toPoint());
        QTest::mouseMove(canvas, transform.map(last.last().center()).toPoint(), 20);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, transform.map(last.last().center()).toPoint());
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), text.all());
        QTest::keyClick(canvas, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), text.all());
        QVERIFY(windows<EditorWindow>().isEmpty());
        draw("箭头", {20, 10}, {100, 20});
        QCOMPARE(overlay->session()->document.state().annotations.size(), 1);
    }
    void instantTranslation() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        delay = 300;
        click("AI 翻译");
        QTRY_VERIFY_WITH_TIMEOUT(requests > 0, 10000);
        draw("箭头", {20, 10}, {100, 20});
        waitForTranslation();
        const auto &document = overlay->session()->document;
        QCOMPARE(document.state().annotations.size(), 1);
        QVERIFY(document.render() != document.original());
        click("复制译文");
        QVERIFY(QApplication::clipboard()->text().contains("你好世界"));
        auto *mode = overlay->findChild<QFrame *>("quickToolbar")->findChild<QComboBox *>();
        QVERIFY(mode);
        mode->setCurrentIndex(2);
        QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 2));
        QCOMPARE(document.state().translationMode, 2);
    }
    void advancedHandoff() {
        capture();
        QVERIFY(overlay);
        draw("箭头");
        click("文字");
        auto *canvas = overlay->canvasWidget();
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, canvas->sourceToWidget().map(QPointF(330, 220)).toPoint());
        auto *input = canvas->findChild<QTextEdit *>("annotationTextInput");
        QVERIFY(input && input->hasFocus());
        QTest::keyClicks(input, "Note");
        QTest::keyClick(input, Qt::Key_Return);
        QTest::keyClicks(input, "Second line");
        QVERIFY(windows<EditorWindow>().isEmpty());
        QVERIFY(canvas->isEditingText());
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
        auto session = overlay->session();
        auto *document = &session->document;
        QCOMPARE(document->state().annotations.size(), 2);
        const auto rendered = document->render();
        QTest::keyClick(canvas, Qt::Key_Return);
        QTRY_COMPARE(windows<EditorWindow>().size(), 1);
        QTRY_VERIFY(overlay.isNull());
        auto *editor = windows<EditorWindow>().first();
        QCOMPARE(editor->document(), document);
        QCOMPARE(document->render(), rendered);
        document->undo();
        QCOMPARE(document->state().annotations.size(), 1);
        document->redo();
        QCOMPARE(document->state().annotations.size(), 2);
        auto *advanced = editor->canvasWidget();
        const auto transform = advanced->sourceToWidget();
        const auto before = document->state().annotations.first().points;
        const QPointF center = document->state().annotations.first().bounds().center();
        QTest::mousePress(advanced, Qt::LeftButton, Qt::NoModifier, transform.map(center).toPoint());
        QTest::mouseMove(advanced, transform.map(center + QPointF(30, 25)).toPoint(), 20);
        QTest::mouseRelease(advanced, Qt::LeftButton, Qt::NoModifier, transform.map(center + QPointF(30, 25)).toPoint());
        QVERIFY(document->state().annotations.first().points != before);
        advanced->setColor(Qt::blue);
        QCOMPARE(document->state().annotations.first().color, QColor(Qt::blue));
        QTest::keyClick(advanced, Qt::Key_Delete);
        QCOMPARE(document->state().annotations.size(), 1);
    }
    void translatedPng() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        click("AI 翻译");
        waitForTranslation();
        auto session = overlay->session();
        const auto path = files.filePath("translated.png");
        QSignalSpy saved(session.get(), &EditorSession::imageSaved);
        click("保存");
        QTRY_VERIFY_WITH_TIMEOUT(!windows<QFileDialog>().isEmpty(), 3000);
        auto *dialog = windows<QFileDialog>().first();
        dialog->selectFile(path);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 5000);
        const auto output = QImage(path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(output, session->document.render());
        QVERIFY(output != session->document.original());
        click("复制图片");
        QCOMPARE(QApplication::clipboard()->image().convertToFormat(QImage::Format_ARGB32_Premultiplied), output);
        QVERIFY(overlay && overlay->isVisible());
        click("保存");
        QTRY_VERIFY_WITH_TIMEOUT(!windows<QFileDialog>().isEmpty(), 3000);
        dialog = windows<QFileDialog>().first();
        auto *fileType = dialog->findChild<QComboBox *>("fileTypeCombo");
        QVERIFY(fileType);
        fileType->setFocus();
        QTest::keyClick(fileType, Qt::Key_End);
        dialog->selectFile(files.filePath("translated-jpeg"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 2, 5000);
        const auto jpeg = files.filePath("translated-jpeg.jpg");
        QVERIFY2(QFileInfo::exists(jpeg), qPrintable(saved.last().first().toString()));
        QImageReader reader(jpeg);
        QCOMPARE(reader.format(), QByteArray("jpeg"));
        QCOMPARE(reader.size(), session->document.render().size());
    }
    void unfinishedOcrHandoff() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        click("OCR");
        auto session = overlay->session();
        click("✅");
        QTRY_COMPARE(windows<EditorWindow>().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!session->document.state().ocr.isEmpty(), 10000);
        QCOMPARE(windows<EditorWindow>().first()->document(), &session->document);
    }
    void unfinishedTranslationHandoff() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        delay = 400;
        click("AI 翻译");
        QTRY_VERIFY_WITH_TIMEOUT(requests > 0, 10000);
        draw("矩形", {20, 10}, {100, 25});
        auto session = overlay->session();
        click("✅");
        QTRY_COMPARE(windows<EditorWindow>().size(), 1);
        QTRY_VERIFY(overlay.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!session->document.state().translations.isEmpty(), 5000);
        QCOMPARE(windows<EditorWindow>().first()->document(), &session->document);
        QCOMPARE(session->document.state().annotations.size(), 1);
    }
    void translationFailureAndCancel() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        responseStatus = 500;
        click("AI 翻译");
        QTRY_VERIFY_WITH_TIMEOUT(overlay->session()->message().contains("HTTP 500"), 10000);
        QVERIFY(windows<QMessageBox>().isEmpty());
        QVERIFY(overlay->session()->document.state().translations.isEmpty());
        draw("箭头", {20, 10}, {100, 20});
        QCOMPARE(overlay->session()->document.state().annotations.size(), 1);
        delay = 300;
        responseStatus = 200;
        click("AI 翻译");
        QTRY_COMPARE_WITH_TIMEOUT(requests, 2, 10000);
        click("❌");
        QTRY_VERIFY(overlay.isNull());
        QTest::qWait(350);
        QVERIFY(windows<EditorWindow>().isEmpty());
    }
    void quitWithPendingTranslation() {
        capture({20, 250, 760, 80});
        QVERIFY(overlay);
        delay = 500;
        click("AI 翻译");
        QTRY_VERIFY_WITH_TIMEOUT(requests > 0, 10000);
        application.reset();
        QTRY_VERIFY(overlay.isNull());
        QTest::qWait(550);
        QVERIFY(windows<EditorWindow>().isEmpty());
        QVERIFY(windows<SelectionOverlay>().isEmpty());
    }
};

int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("XDG_CACHE_HOME", config.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    qputenv("QT_XCB_GL_INTEGRATION", "none");
    qputenv("QT_USE_PORTAL", "0");
    qputenv("SNAPLING_TEST_X11", "1");
    qputenv("XDG_SESSION_TYPE", "x11");
    XInitThreads();
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
    app.setApplicationName("SnapLingQuickEditTest");
    app.setQuitOnLastWindowClosed(false);
    TestQuickEdit test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestQuickEdit.moc"

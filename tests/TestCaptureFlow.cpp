#include "app/Application.h"
#include "selection/SelectionOverlay.h"
#include "ui/EditorWindow.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace SnapLing;

// 只在测试启动的独立 D-Bus 会话中模拟系统截图响应。
class ScreenshotPortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
    Q_PROPERTY(uint version READ version CONSTANT)
  public:
    uint version() const {
        return 2;
    }
    QString uri;
    uint nonInteractiveCode = 2;
    QList<bool> requests;

  public slots:
    QDBusObjectPath Screenshot(const QString &, const QVariantMap &options) {
        const bool interactive = options.value("interactive").toBool();
        requests << interactive;
        auto bus = QDBusConnection::sessionBus();
        const auto sender = bus.baseService().mid(1).replace('.', '_');
        const auto path = "/org/freedesktop/portal/desktop/request/" + sender + "/" +
                          options.value("handle_token").toString();
        const uint code = interactive ? 0 : nonInteractiveCode;
        QTimer::singleShot(0, this, [bus, path, code, uri = uri] {
            auto response = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            response.setArguments({code, QVariantMap{{"uri", uri}}});
            bus.send(response);
        });
        return QDBusObjectPath(path);
    }
};

class TestCaptureFlow : public QObject {
    Q_OBJECT
  private:
    ScreenshotPortal portal;
    QTemporaryDir images;
    QImage image;

    template <class T> static QList<T *> windows() {
        QList<T *> result;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<T *>(widget))
                result << window;
        return result;
    }

  private slots:
    void initTestCase() {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
        QVERIFY(bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                                   QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        QVERIFY(images.isValid());
        image = QImage(180, 120, QImage::Format_RGB32);
        image.fill(QColor("#20c060"));
        image.setPixelColor(10, 20, Qt::red);
        const auto path = images.filePath("selected.png");
        QVERIFY(image.save(path));
        portal.uri = QUrl::fromLocalFile(path).toString();
        Settings settings;
        settings.set("hotkey/capture", "");
    }
    void init() {
        portal.requests.clear();
        portal.nonInteractiveCode = 2;
    }
    void cleanup() {
        for (auto *overlay : windows<SelectionOverlay>())
            overlay->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void portalImageRouting_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<int>("shape");
        QTest::addColumn<uint>("nonInteractiveCode");
        QTest::addColumn<bool>("directToEditor");
        QTest::addColumn<int>("requestCount");
        QTest::newRow("interactive-region") << "--capture" << -1 << uint(2) << true << 2;
        QTest::newRow("interactive-window") << "--window" << -1 << uint(2) << true << 1;
        QTest::newRow("noninteractive-region") << "--capture" << -1 << uint(0) << false << 1;
        QTest::newRow("fullscreen") << "--full" << -1 << uint(0) << true << 1;
        QTest::newRow("interactive-fullscreen") << "--full" << -1 << uint(2) << true << 2;
        QTest::newRow("ellipse") << "" << int(CaptureMode::Ellipse) << uint(2) << false << 2;
        QTest::newRow("polygon") << "" << int(CaptureMode::Polygon) << uint(2) << false << 2;
        QTest::newRow("freehand") << "" << int(CaptureMode::Freehand) << uint(2) << false << 2;
    }
    void portalImageRouting() {
        QFETCH(QString, command);
        QFETCH(int, shape);
        QFETCH(uint, nonInteractiveCode);
        QFETCH(bool, directToEditor);
        QFETCH(int, requestCount);
        portal.nonInteractiveCode = nonInteractiveCode;
        Application app;
        if (shape < 0)
            app.command({command});
        else {
            app.show();
            auto *main = windows<QMainWindow>().first();
            auto *modes = main->findChild<QComboBox *>();
            QVERIFY(modes);
            modes->setCurrentIndex(shape);
            QPushButton *capture = nullptr;
            for (auto *button : main->findChildren<QPushButton *>())
                if (button->text() == "开始截图")
                    capture = button;
            QVERIFY(capture);
            capture->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!windows<EditorWindow>().isEmpty() ||
                                     !windows<SelectionOverlay>().isEmpty(), 5000);
        QCOMPARE(portal.requests.size(), requestCount);
        QCOMPARE(portal.requests.first(), command == "--window");
        QCOMPARE(portal.requests.last(), requestCount == 2 || command == "--window");
        QCOMPARE(windows<EditorWindow>().size(), directToEditor ? 1 : 0);
        QCOMPARE(windows<SelectionOverlay>().size(), directToEditor ? 0 : 1);
        if (directToEditor)
            QCOMPARE(windows<EditorWindow>().first()->document()->render(),
                     image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    void cancelThenRetry() {
        portal.nonInteractiveCode = 1;
        Application app;
        app.command({"--capture"});
        QTRY_VERIFY_WITH_TIMEOUT(windows<QMainWindow>().first()->isVisible(), 5000);
        QCOMPARE(portal.requests.size(), 1);
        QVERIFY(windows<EditorWindow>().isEmpty());
        QVERIFY(windows<SelectionOverlay>().isEmpty());
        portal.nonInteractiveCode = 2;
        app.command({"--capture"});
        QTRY_VERIFY_WITH_TIMEOUT(!windows<EditorWindow>().isEmpty() ||
                                     !windows<SelectionOverlay>().isEmpty(), 5000);
        QCOMPARE(portal.requests.size(), 3);
        QCOMPARE(windows<EditorWindow>().size(), 1);
        QVERIFY(windows<SelectionOverlay>().isEmpty());
    }
    void cleanupTestCase() {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject("/org/freedesktop/portal/desktop");
        bus.unregisterService("org.freedesktop.portal.Desktop");
    }
};

int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("XDG_CACHE_HOME", config.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "minimal");
    qputenv("XDG_SESSION_TYPE", "wayland");
    qunsetenv("SNAPLING_TEST_X11");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
    app.setApplicationName("SnapLingCaptureFlowTest");
    app.setQuitOnLastWindowClosed(false);
    TestCaptureFlow test;
    return QTest::qExec(&test, argc, argv);
}

#include "TestCaptureFlow.moc"

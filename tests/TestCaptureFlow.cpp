#include "app/Application.h"
#include "selection/SelectionOverlay.h"
#include "ui/EditorWindow.h"
#include "platform/linux/wayland/Portal.h"
#include "platform/linux/wayland/WaylandCapture.h"
#ifdef SNAPLING_HAVE_PIPEWIRE
#include "platform/linux/wayland/PipeWireStream.h"
#endif
#include <QApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QFrame>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <fcntl.h>
#include <unistd.h>

using namespace SnapLing;
static const QString portalService = "org.freedesktop.portal.Desktop";
static const QString portalRoot = "/org/freedesktop/portal/desktop";
struct VideoNode { uint id; QVariantMap properties; };
using VideoNodes = QList<VideoNode>;
struct CoordinatePair { int first, second; };
Q_DECLARE_METATYPE(VideoNode)
Q_DECLARE_METATYPE(VideoNodes)
Q_DECLARE_METATYPE(CoordinatePair)
QDBusArgument &operator<<(QDBusArgument &arg, const CoordinatePair &pair) {
    arg.beginStructure();
    arg << pair.first << pair.second;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, CoordinatePair &pair) {
    arg.beginStructure();
    arg >> pair.first >> pair.second;
    arg.endStructure();
    return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const VideoNode &node) {
    arg.beginStructure();
    arg << node.id << node.properties;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, VideoNode &node) {
    arg.beginStructure();
    arg >> node.id >> node.properties;
    arg.endStructure();
    return arg;
}
// 测试专属 D-Bus 服务，所有屏幕数据都由当前测试创建。
static QDBusObjectPath respond(QObject *context, const QVariantMap &options, uint code,
                               QVariantMap results, int delay = 0) {
    auto bus = QDBusConnection::sessionBus();
    const auto sender = bus.baseService().mid(1).replace('.', '_');
    const auto path = portalRoot + "/request/" + sender + "/" + options["handle_token"].toString();
    QTimer::singleShot(delay, context, [bus, path, code, results] {
        auto response = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
        response.setArguments({code, results});
        bus.send(response);
    });
    return QDBusObjectPath(path);
}
class TestSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
  public:
    TestSession(int &counter, QObject *parent) : QObject(parent), closed(counter) {}
    int &closed;
  public slots:
    void Close() { ++closed; }
};
class ScreenshotPortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
    Q_PROPERTY(uint version READ version CONSTANT)
  public:
    uint version() const { return 2; }
    QString uri;
    uint code = 0;
    QList<bool> requests;
  public slots:
    QDBusObjectPath Screenshot(const QString &, const QVariantMap &options) {
        requests << options["interactive"].toBool();
        return respond(this, options, code, {{"uri", uri}});
    }
};
class ScreenCastPortal : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
    Q_PROPERTY(uint version READ version CONSTANT)
  public:
    explicit ScreenCastPortal(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 4; }
    int created = 0, closed = 0, started = 0, opened = 0, startDelay = 0;
    uint startCode = 0;
    bool invalidStreams = false;
    QRect desktop;
    QVariantMap sourceOptions;
    QStringList sessions;
  public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options) {
        auto bus = QDBusConnection::sessionBus();
        const auto sender = bus.baseService().mid(1).replace('.', '_');
        const auto path = portalRoot + "/session/" + sender + "/" + options["session_handle_token"].toString();
        bus.registerObject(path, new TestSession(closed, this), QDBusConnection::ExportAllSlots);
        sessions << path;
        ++created;
        return respond(this, options, 0, {{"session_handle", QVariant::fromValue(QDBusObjectPath(path))}});
    }
    QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options) {
        sourceOptions = options;
        return respond(this, options, 0, {});
    }
    QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &options) {
        ++started;
        QVariantMap properties{{"pipewire-serial", "42"}};
        if (!desktop.isEmpty()) {
            properties["position"] = QVariant::fromValue(CoordinatePair{desktop.x(), desktop.y()});
            properties["size"] = QVariant::fromValue(CoordinatePair{desktop.width(), desktop.height()});
        }
        const VideoNodes nodes{{37, properties}};
        return respond(this, options, startCode,
                       {{"streams", invalidStreams ? QVariant("invalid") : QVariant::fromValue(nodes)},
                        {"restore_token", "test_restore"}}, startDelay);
    }
    QDBusUnixFileDescriptor OpenPipeWireRemote(const QDBusObjectPath &, const QVariantMap &) {
        ++opened;
        const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QDBusUnixFileDescriptor result(descriptor);
        ::close(descriptor);
        return result;
    }
};
#ifdef SNAPLING_HAVE_PIPEWIRE
struct StreamControl {
    QImage frame;
    int started = 0, stopped = 0;
    bool descriptorValid = false;
};
class TestStream : public PipeWireStream {
  public:
    explicit TestStream(std::shared_ptr<StreamControl> control) : control(std::move(control)) {}
    bool start(int descriptor, uint node, const QString &serial, QString &) override {
        control->descriptorValid = ::fcntl(descriptor, F_GETFD) >= 0 && node == 37 && serial == "42";
        ::close(descriptor);
        ++control->started;
        return true;
    }
    void stop() override { ++control->stopped; }
    QImage latest() const override { return control->frame; }
  private:
    std::shared_ptr<StreamControl> control;
};
#endif
class TestCaptureFlow : public QObject {
    Q_OBJECT
  private:
    ScreenshotPortal screenshot;
    ScreenCastPortal screenCast{&screenshot};
    QTemporaryDir images;
    QImage image;
#ifdef SNAPLING_HAVE_PIPEWIRE
    std::shared_ptr<StreamControl> control;
#endif
    template <class T> static QList<T *> windows() {
        QList<T *> result;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<T *>(widget))
                result << window;
        return result;
    }
    std::unique_ptr<WaylandCapture> backend() {
#ifdef SNAPLING_HAVE_PIPEWIRE
        return std::make_unique<WaylandCapture>(nullptr, [state = control] {
            return std::make_unique<TestStream>(state);
        });
#else
        return std::make_unique<WaylandCapture>();
#endif
    }
  private slots:
    void initTestCase() {
        qDBusRegisterMetaType<VideoNode>();
        qDBusRegisterMetaType<VideoNodes>();
        qDBusRegisterMetaType<CoordinatePair>();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerService(portalService));
        QVERIFY(bus.registerObject(portalRoot, &screenshot,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAdaptors));
        QCOMPARE(PortalRequest::version("org.freedesktop.portal.ScreenCast"), uint(4));
        QVERIFY(images.isValid());
        image = QImage(QGuiApplication::primaryScreen()->size(), QImage::Format_RGB32);
        image.fill(QColor("#20c060"));
        image.setPixelColor(10, 20, Qt::red);
        const auto path = images.filePath("desktop.png");
        QVERIFY(image.save(path));
        screenshot.uri = QUrl::fromLocalFile(path).toString();
        Settings settings;
        settings.set("hotkey/capture", "");
    }
    void init() {
        screenshot.requests.clear();
        screenshot.code = 0;
        screenCast.created = screenCast.closed = screenCast.started = screenCast.opened = 0;
        screenCast.startCode = 0;
        screenCast.startDelay = 0;
        screenCast.invalidStreams = false;
        screenCast.desktop = {};
        screenCast.sourceOptions.clear();
        QSettings().remove("wayland/restoreToken");
#ifdef SNAPLING_HAVE_PIPEWIRE
        control = std::make_shared<StreamControl>();
        control->frame = image;
#endif
    }
    void cleanup() {
        for (auto *overlay : windows<SelectionOverlay>())
            overlay->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_COMPARE(screenCast.closed, screenCast.created);
        for (const auto &path : screenCast.sessions)
            QDBusConnection::sessionBus().unregisterObject(path);
        screenCast.sessions.clear();
    }
    void fullFrameBeforeSelection_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<int>("shape");
        QTest::addColumn<bool>("quickReady");
        QTest::newRow("region") << "--capture" << -1 << false;
        QTest::newRow("window") << "--window" << -1 << false;
        QTest::newRow("fullscreen") << "--full" << -1 << true;
        QTest::newRow("ellipse") << "" << int(CaptureMode::Ellipse) << false;
        QTest::newRow("polygon") << "" << int(CaptureMode::Polygon) << false;
        QTest::newRow("freehand") << "" << int(CaptureMode::Freehand) << false;
    }
    void fullFrameBeforeSelection() {
        QFETCH(QString, command);
        QFETCH(int, shape);
        QFETCH(bool, quickReady);
        Application app(nullptr, backend());
        if (shape < 0)
            app.command({command});
        else {
            app.show();
            auto *main = windows<QMainWindow>().first();
            main->findChild<QComboBox *>()->setCurrentIndex(shape);
            for (auto *button : main->findChildren<QPushButton *>())
                if (button->text() == "开始截图")
                    button->click();
        }
        QTRY_COMPARE_WITH_TIMEOUT(windows<SelectionOverlay>().size(), 1, 5000);
#ifdef SNAPLING_HAVE_PIPEWIRE
        QVERIFY(screenshot.requests.isEmpty());
        QCOMPARE(screenCast.created, 1);
        QCOMPARE(screenCast.opened, 1);
        QCOMPARE(screenCast.sourceOptions["types"].toUInt(), uint(1));
        QVERIFY(!screenCast.sourceOptions["multiple"].toBool());
        QVERIFY(control->descriptorValid);
        QCOMPARE(control->stopped, 1);
        QTRY_COMPARE(screenCast.closed, 1);
#else
        QCOMPARE(screenshot.requests, QList<bool>{false});
#endif
        QVERIFY(windows<EditorWindow>().isEmpty());
        auto *overlay = windows<SelectionOverlay>().first();
        const auto id = overlay->winId();
        if (!quickReady) {
            QCOMPARE(overlay->phase(), SelectionOverlay::Phase::Selecting);
            QVERIFY(!overlay->canvasWidget());
            if (shape < 0) {
                QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, {25, 40});
                QVERIFY(!overlay->canvasWidget());
                QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, {150, 190});
                quickReady = true;
            }
        }
        if (quickReady) {
            QTRY_VERIFY(overlay->canvasWidget());
            QCOMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
            QCOMPARE(overlay->winId(), id);
            auto *toolbar = overlay->findChild<QFrame *>("quickToolbar");
            QVERIFY(toolbar && toolbar->isVisible());
            QCOMPARE(toolbar->parentWidget(), overlay);
            QVERIFY(windows<EditorWindow>().isEmpty());
            auto *document = &overlay->session()->document;
            QTest::keyClick(overlay->canvasWidget(), Qt::Key_Return);
            QTRY_COMPARE(windows<EditorWindow>().size(), 1);
            QCOMPARE(windows<EditorWindow>().first()->document(), document);
        }
    }
#ifdef SNAPLING_HAVE_PIPEWIRE
    void portalDesktopCoordinates() {
        screenCast.desktop = QRect(-1280, 180, 1280, 720);
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        const auto frame = qvariant_cast<CaptureFrame>(frames.first().first());
        QCOMPARE(frame.desktop, QRectF(screenCast.desktop));
        QCOMPARE(frame.image, image);
        QTRY_COMPARE(screenCast.closed, 1);
    }
    void malformedStreamsFailSafely() {
        screenCast.invalidStreams = true;
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        QSignalSpy failed(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(frames.isEmpty());
        QVERIFY(screenshot.requests.isEmpty());
        QCOMPARE(screenCast.opened, 0);
        QTRY_COMPARE(screenCast.closed, 1);
    }
    void delayedFirstFrame() {
        control->frame = {};
        Application app(nullptr, backend());
        app.command({"--capture"});
        QTRY_COMPARE(screenCast.opened, 1);
        QTest::qWait(50);
        QVERIFY(windows<SelectionOverlay>().isEmpty());
        control->frame = image;
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QCOMPARE(windows<SelectionOverlay>().first()->phase(), SelectionOverlay::Phase::Selecting);
        QTRY_COMPARE(screenCast.closed, 1);
        QVERIFY(screenshot.requests.isEmpty());
    }
    void deniedThenRetry() {
        screenCast.startCode = 1;
        Application app(nullptr, backend());
        app.command({"--capture"});
        QTRY_VERIFY(windows<QMainWindow>().first()->isVisible());
        QVERIFY(windows<SelectionOverlay>().isEmpty());
        QVERIFY(screenshot.requests.isEmpty());
        screenCast.startCode = 0;
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QCOMPARE(windows<SelectionOverlay>().first()->phase(), SelectionOverlay::Phase::Selecting);
        QTRY_COMPARE(screenCast.closed, 2);
    }
    void cancelBeforeFirstFrame() {
        control->frame = {};
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_VERIFY(capture->sharing());
        capture->cancel();
        control->frame = image;
        QTest::qWait(60);
        QVERIFY(frames.isEmpty());
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QTRY_COMPARE(screenCast.closed, 2);
        QVERIFY(screenshot.requests.isEmpty());
    }
    void lateAuthorizationAfterRetry() {
        screenCast.startDelay = 100;
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(screenCast.started, 1);
        capture->cancel();
        screenCast.startDelay = 0;
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QTest::qWait(150);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(control->started, 1);
        QTRY_COMPARE(screenCast.closed, 2);
    }
    void explicitlySharedSourceStaysActive() {
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->startSharing();
        QTRY_VERIFY(capture->sharing());
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QVERIFY(capture->sharing());
        QCOMPARE(control->stopped, 0);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(screenCast.created, 1);
        capture->stopSharing();
        QTRY_COMPARE(screenCast.closed, 1);
        QCOMPARE(control->stopped, 1);
    }
#else
    void deniedDoesNotOpenSystemSelector() {
        screenshot.code = 2;
        auto capture = backend();
        QSignalSpy failed(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(screenshot.requests, QList<bool>{false});
        QVERIFY(windows<SelectionOverlay>().isEmpty());
    }
#endif
    void cleanupTestCase() {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(portalRoot);
        bus.unregisterService(portalService);
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

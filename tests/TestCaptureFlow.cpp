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
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QFrame>
#include <QClipboard>
#include <QPainter>
#include <QProcess>
#include <QWindow>
#include <QToolButton>
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
                               QVariantMap results, int delay = 0, QString caller = {}) {
    auto bus = QDBusConnection::sessionBus();
    const auto sender = (caller.isEmpty() ? bus.baseService() : caller).mid(1).replace('.', '_');
    const auto path = portalRoot + "/request/" + sender + "/" + options["handle_token"].toString();
    QTimer::singleShot(delay, context, [bus, path, code, results, caller] {
        auto response = caller.isEmpty()
                            ? QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response")
                            : QDBusMessage::createTargetedSignal(caller, path, "org.freedesktop.portal.Request", "Response");
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
using PermissionMap = QMap<QString, QStringList>;
class PermissionStorePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.PermissionStore")
  public:
    PermissionMap records;
    int writes = 0;
    bool rejectWrite = false;
    QString writtenApp;
    QStringList writtenPermissions;
  public slots:
    void Lookup(const QString &, const QString &, PermissionMap &permissions, QDBusVariant &data) {
        permissions = records;
        data = QDBusVariant(QString("unchanged-test-metadata"));
    }
    void SetPermission(const QString &table, bool create, const QString &id,
                       const QString &app, const QStringList &permissions) {
        if (rejectWrite) {
            sendErrorReply("org.freedesktop.DBus.Error.AccessDenied", "test refusal");
            return;
        }
        Q_ASSERT(table == "screenshot" && create && id == "screenshot");
        ++writes;
        writtenApp = app;
        writtenPermissions = permissions;
        records[app] = permissions;
    }
};
struct RegistryState {
    QMap<QString, QString> identities;
    QStringList operations, registeredSenders, screenshotSenders;
    bool supported = true, fail = false, cacheOnFailure = false;
    int delay = 0;
};
class RegistryPortal : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.host.portal.Registry")
  public:
    RegistryPortal(QObject *parent, RegistryState &state) : QDBusAbstractAdaptor(parent), state(state) {}
    RegistryState &state;
  public slots:
    void Register(const QString &appId, const QVariantMap &, const QDBusMessage &call) {
        const auto sender = call.service();
        state.operations << "Register:" + sender;
        call.setDelayedReply(true);
        auto error = [call](QString name, QString message) {
            QDBusConnection::sessionBus().send(call.createErrorReply(name, message));
        };
        if (!state.supported) {
            error("org.freedesktop.DBus.Error.UnknownInterface", "test legacy portal");
            return;
        }
        if (state.identities.contains(sender)) {
            error("org.freedesktop.portal.Error.Failed", "Connection already associated with an application ID");
            return;
        }
        if (state.fail) {
            if (state.cacheOnFailure)
                state.identities[sender] = appId;
            error("org.freedesktop.portal.Error.Failed", "test registry failure");
            return;
        }
        QTimer::singleShot(state.delay, this, [this, call, sender, appId] {
            state.identities[sender] = appId;
            state.registeredSenders << sender;
            QDBusConnection::sessionBus().send(call.createReply());
        });
    }
};
class ScreenshotPortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
    Q_PROPERTY(uint version READ version CONSTANT)
  public:
    uint version() const { return 2; }
    QString uri;
    uint code = 0;
    int delay = 0;
    QList<bool> requests;
    PermissionStorePortal *permissions = nullptr;
    RegistryState *registry = nullptr;
    bool respectPermissions = false;
  public slots:
    QDBusObjectPath Screenshot(const QString &, const QVariantMap &options) {
        requests << options["interactive"].toBool();
        const auto caller = message().service();
        // 模拟真实 Portal：第一个请求固定该唯一连接的身份，之后不能再 Register。
        if (!registry->identities.contains(caller))
            registry->identities[caller] = {};
        registry->operations << "Screenshot:" + caller;
        registry->screenshotSenders << caller;
        const auto id = registry->identities.value(caller);
        const bool denied = respectPermissions && permissions->records.value(id).contains("no");
        const uint response = denied ? 2 : code;
        return respond(this, options, response, response ? QVariantMap{} : QVariantMap{{"uri", uri}}, delay, caller);
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
    VideoNodes nodeList;
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
        const VideoNodes nodes = nodeList.isEmpty() ? VideoNodes{{37, properties}} : nodeList;
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
    QHash<uint, QImage> frames;
    QList<uint> startedNodes;
    int started = 0, stopped = 0;
    QSet<uint> activeNodes;
    QList<uint> requestedNodes;
    bool descriptorValid = true;
};
class TestStream : public PipeWireStream {
  public:
    explicit TestStream(std::shared_ptr<StreamControl> control) : control(std::move(control)) {}
    bool start(int descriptor, uint node, const QString &serial, QString &) override {
        this->node = node;
        control->descriptorValid &= ::fcntl(descriptor, F_GETFD) >= 0 && serial == QString::number(node + 5);
        ::close(descriptor);
        ++control->started;
        control->startedNodes << node;
        return true;
    }
    void stop() override { ++control->stopped; }
    void setActive(bool active) override {
        if (active) {
            control->activeNodes.insert(node);
            control->requestedNodes << node;
        } else
            control->activeNodes.remove(node);
    }
    QImage latest() const override { return control->frames.contains(node) ? control->frames.value(node) : control->frame; }
  private:
    std::shared_ptr<StreamControl> control;
    uint node = 0;
};
#endif
class TestCaptureFlow : public QObject {
    Q_OBJECT
  private:
    ScreenshotPortal screenshot;
    RegistryState registryState;
    RegistryPortal registry{&screenshot, registryState};
    PermissionStorePortal permissionStore;
    ScreenCastPortal screenCast{&screenshot};
    QTemporaryDir images;
    QImage image;
    QVector<CaptureScreen> testLayout;
#ifdef SNAPLING_HAVE_PIPEWIRE
    std::shared_ptr<StreamControl> control;
    static VideoNode node(uint id, const CaptureScreen &screen) {
        return {id, {{"id", QString::number(id)}, {"source_type", uint(1)},
                     {"pipewire-serial", QString::number(id + 5)},
                     {"position", QVariant::fromValue(CoordinatePair{qRound(screen.desktop.x()), qRound(screen.desktop.y())})},
                     {"size", QVariant::fromValue(CoordinatePair{qRound(screen.desktop.width()), qRound(screen.desktop.height())})}}};
    }
    void threeScreens() {
        testLayout = {{"eDP-1", {0, 0, 960, 600}, {}, 1},
                      {"HDMI-A-1", {-1280, 0, 1280, 720}, {}, 1.5},
                      {"DP-1", {960, -180, 800, 600}, {}, 2}};
        const QList<QColor> colors{QColor("#20c060"), QColor("#2458d8"), QColor("#dc3b4b")};
        for (int i = 0; i < testLayout.size(); ++i) {
            QImage frame((testLayout[i].desktop.size() * testLayout[i].devicePixelRatio).toSize(), QImage::Format_RGB32);
            frame.fill(colors[i]);
            control->frames[uint(37 + i)] = frame;
        }
        // Portal 返回顺序与 Qt 的显示器顺序不同，防止错误依赖第一个节点。
        screenCast.nodeList = {node(39, testLayout[2]), node(37, testLayout[0]), node(38, testLayout[1])};
    }
#endif
    template <class T> static QList<T *> windows() {
        QList<T *> result;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<T *>(widget))
                result << window;
        return result;
    }
    std::unique_ptr<WaylandCapture> backend(bool compatibility = true) {
#ifdef SNAPLING_HAVE_PIPEWIRE
        auto result = std::make_unique<WaylandCapture>(nullptr, [state = control] {
            return std::make_unique<TestStream>(state);
        }, [this] { return testLayout; });
        result->setMethod(compatibility ? WaylandCapture::Method::ScreenCast : WaylandCapture::Method::Screenshot);
        return result;
#else
        Q_UNUSED(compatibility)
        return std::make_unique<WaylandCapture>(nullptr, WaylandCapture::StreamFactory{}, [this] { return testLayout; });
#endif
    }
  private slots:
    void initTestCase() {
        qDBusRegisterMetaType<VideoNode>();
        qDBusRegisterMetaType<VideoNodes>();
        qDBusRegisterMetaType<CoordinatePair>();
        qDBusRegisterMetaType<PermissionMap>();
        qRegisterMetaType<PermissionMap>("PermissionMap");
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerService(portalService));
        QVERIFY(bus.registerService("org.freedesktop.impl.portal.PermissionStore"));
        QVERIFY(bus.registerObject("/org/freedesktop/impl/portal/PermissionStore", &permissionStore,
                                  QDBusConnection::ExportAllSlots));
        screenshot.permissions = &permissionStore;
        screenshot.registry = &registryState;
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
        screenshot.respectPermissions = false;
        registryState = {};
        permissionStore.records.clear();
        permissionStore.writes = 0;
        permissionStore.rejectWrite = false;
        screenshot.delay = 0;
        screenshot.uri = QUrl::fromLocalFile(images.filePath("desktop.png")).toString();
        screenCast.created = screenCast.closed = screenCast.started = screenCast.opened = 0;
        screenCast.startCode = 0;
        screenCast.startDelay = 0;
        screenCast.invalidStreams = false;
        screenCast.desktop = {};
        screenCast.nodeList.clear();
        testLayout = captureScreenLayout();
        screenCast.sourceOptions.clear();
        QSettings().remove("wayland/restoreToken");
        QSettings().remove("wayland/multiScreenRestoreToken");
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
        QVERIFY(screenCast.sourceOptions["multiple"].toBool());
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
    void allThreeScreensAndMixedDpi() {
        threeScreens();
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        const auto frame = qvariant_cast<CaptureFrame>(frames.first().first());
        QCOMPARE(frame.screens.size(), 3);
        QCOMPARE(frame.desktop, QRectF(-1280, -180, 3040, 900));
        QCOMPARE(frame.image.size(), QSize(6080, 1800));
        for (int i = 0; i < testLayout.size(); ++i) {
            const auto at = frame.toPixel(testLayout[i].desktop.center()).toPoint();
            QCOMPARE(frame.image.pixelColor(at), control->frames.value(uint(37 + i)).pixelColor(0, 0));
        }
        QCOMPARE(control->started, 3);
        QCOMPARE(control->stopped, 3);
        QCOMPARE(screenCast.opened, 3);
        QVERIFY(control->descriptorValid);
        QVERIFY(screenCast.sourceOptions["multiple"].toBool());
        QTRY_COMPARE(screenCast.closed, 1);
    }
    void selectedExternalIgnoresPortalOrder_data() {
        QTest::addColumn<int>("screen");
        QTest::newRow("hdmi") << 1;
        QTest::newRow("displayport") << 2;
    }
    void selectedExternalIgnoresPortalOrder() {
        QFETCH(int, screen);
        threeScreens();
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region, screen);
        QTRY_COMPARE(frames.size(), 1);
        const auto frame = qvariant_cast<CaptureFrame>(frames.first().first());
        QCOMPARE(frame.screenName, testLayout[screen].name);
        QCOMPARE(frame.desktop, testLayout[screen].desktop);
        QCOMPARE(frame.image, control->frames.value(uint(37 + screen)));
        QCOMPARE(frame.screens.size(), 1);
        QCOMPARE(control->startedNodes, QList<uint>{uint(37 + screen)});
        QCOMPARE(screenCast.opened, 1);
    }
    void sameResolutionScreensUsePosition() {
        threeScreens();
        testLayout[1].desktop = {-960, 0, 960, 600};
        testLayout[2].desktop = {960, 0, 960, 600};
        screenCast.nodeList = {node(39, testLayout[2]), node(37, testLayout[0]), node(38, testLayout[1])};
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region, 1);
        QTRY_COMPARE(frames.size(), 1);
        const auto frame = qvariant_cast<CaptureFrame>(frames.first().first());
        QCOMPARE(frame.screenName, QString("HDMI-A-1"));
        QCOMPARE(frame.desktop, testLayout[1].desktop);
        QCOMPARE(frame.image, control->frames.value(38));
    }
    void allScreensWaitForExternal() {
        threeScreens();
        const auto external = control->frames.value(39);
        control->frames[39] = {};
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(control->started, 3);
        QTest::qWait(60);
        QVERIFY(frames.isEmpty());
        control->frames[39] = external;
        QTRY_COMPARE(frames.size(), 1);
        QCOMPARE(qvariant_cast<CaptureFrame>(frames.first().first()).screens.size(), 3);
        QCOMPARE(control->stopped, 3);
    }
    void missingAuthorizationCanBeReselected() {
        threeScreens();
        const auto all = screenCast.nodeList;
        screenCast.nodeList = {node(37, testLayout[0])};
        Application app(nullptr, backend());
        app.command({"--capture"});
        QTRY_COMPARE(screenCast.started, 1);
        QTRY_COMPARE(screenCast.closed, 1);
        QVERIFY(windows<SelectionOverlay>().isEmpty());
        QCOMPARE(control->started, 0);
        auto *main = windows<QMainWindow>().first();
        QVERIFY(main->isVisible());
        screenCast.nodeList = all;
        bool clicked = false;
        for (auto *button : main->findChildren<QPushButton *>())
            if (button->objectName() == "resetCompatibilityCapture") {
                button->click();
                clicked = true;
                break;
            }
        QVERIFY(clicked);
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 3);
        QVERIFY(!screenCast.sourceOptions.contains("restore_token"));
        QTRY_COMPARE(screenCast.closed, 2);
    }
    void unknownMultiScreenGeometryFailsSafely() {
        threeScreens();
        for (auto &source : screenCast.nodeList) {
            source.properties.remove("position");
            source.properties.remove("size");
        }
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured), failed(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region, 2);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(frames.isEmpty());
        QCOMPARE(control->started, 0);
    }
    void sharedSourcesSwitchExternalScreens() {
        threeScreens();
        auto capture = backend();
        capture->startSharing();
        QTRY_VERIFY(capture->sharing());
        QVERIFY(control->activeNodes.isEmpty());
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        for (int screen : {1, 2}) {
            control->requestedNodes.clear();
            capture->capture(CaptureMode::Region, screen);
            QTRY_COMPARE(frames.size(), screen);
            const auto frame = qvariant_cast<CaptureFrame>(frames.last().first());
            QCOMPARE(frame.image, control->frames.value(uint(37 + screen)));
            QVERIFY(capture->sharing());
            QVERIFY(control->activeNodes.isEmpty());
            QCOMPARE(control->requestedNodes, QList<uint>{uint(37 + screen)});
        }
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 3);
        QCOMPARE(qvariant_cast<CaptureFrame>(frames.last().first()).screens.size(), 3);
        QCOMPARE(screenCast.created, 1);
        QCOMPARE(screenCast.opened, 3);
        QCOMPARE(control->stopped, 0);
        capture->stopSharing();
        QTRY_COMPARE(screenCast.closed, 1);
        QCOMPARE(control->stopped, 3);
    }
    void externalOverlayKeepsNativePixelsAndAnnotations() {
        threeScreens();
        Application app(nullptr, backend());
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 3);
        SelectionOverlay *external = nullptr;
        for (auto *overlay : windows<SelectionOverlay>())
            if (overlay->captureScreenName() == "DP-1")
                external = overlay;
        QVERIFY(external);
        QTest::mousePress(external, Qt::LeftButton, Qt::NoModifier, {40, 40});
        QTest::mouseRelease(external, Qt::LeftButton, Qt::NoModifier, {240, 180});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QCOMPARE(external->phase(), SelectionOverlay::Phase::QuickEditing);
        auto session = external->session();
        QCOMPARE(session->document.original().size(), QSize(400, 280));
        QCOMPARE(session->document.original().pixelColor(10, 10), control->frames.value(39).pixelColor(0, 0));
        auto *canvas = external->canvasWidget();
        canvas->setTool(Tool::Arrow);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, {20, 20});
        QTest::mouseMove(canvas, {90, 65}, 20);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, {90, 65});
        QCOMPARE(session->document.state().annotations.size(), 1);
        QTest::keyClick(canvas, Qt::Key_Return);
        QTRY_VERIFY(windows<SelectionOverlay>().isEmpty());
        QTRY_COMPARE(windows<EditorWindow>().size(), 1);
        QCOMPARE(windows<EditorWindow>().first()->document(), &session->document);
        QCOMPARE(session->document.state().annotations.size(), 1);
    }
    void cancellingAnyOverlayClosesAllAndCanRetry() {
        threeScreens();
        Application app(nullptr, backend());
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 3);
        QTest::keyClick(windows<SelectionOverlay>().first(), Qt::Key_Escape);
        QTRY_VERIFY(windows<SelectionOverlay>().isEmpty());
        QVERIFY(windows<EditorWindow>().isEmpty());
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 3);
        QCOMPARE(screenCast.created, 2);
    }
    void fullDesktopContainsAllScreens() {
        threeScreens();
        Application app(nullptr, backend());
        app.command({"--full"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        auto *overlay = windows<SelectionOverlay>().first();
        QTRY_COMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
        const auto session = overlay->session();
        QCOMPARE(session->document.original().size(), QSize(6080, 1800));
        CaptureFrame mapping;
        mapping.desktop = {-1280, -180, 3040, 900};
        mapping.image = session->document.original();
        for (int i = 0; i < testLayout.size(); ++i)
            QCOMPARE(mapping.image.pixelColor(mapping.toPixel(testLayout[i].desktop.center()).toPoint()),
                     control->frames.value(uint(37 + i)).pixelColor(0, 0));
    }
    void legacySingleScreenTokenIsNotReused() {
        QSettings settings;
        settings.setValue("wayland/restoreToken", "old_internal_screen");
        auto capture = backend();
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QVERIFY(!screenCast.sourceOptions.contains("restore_token"));
        QCOMPARE(settings.value("wayland/multiScreenRestoreToken").toString(), QString("test_restore"));
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(screenCast.sourceOptions["restore_token"].toString(), QString("test_restore"));
        capture->resetScreenSelection();
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 3);
        QVERIFY(!screenCast.sourceOptions.contains("restore_token"));
    }
    void changedLayoutReauthorizesSharedSources() {
        threeScreens();
        auto capture = backend();
        capture->startSharing();
        QTRY_VERIFY(capture->sharing());
        testLayout[2].desktop.translate(300, 0);
        screenCast.nodeList[0] = node(39, testLayout[2]);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region, 2);
        QTRY_COMPARE(frames.size(), 1);
        QCOMPARE(qvariant_cast<CaptureFrame>(frames.first().first()).desktop, testLayout[2].desktop);
        QVERIFY(!screenCast.sourceOptions.contains("restore_token"));
        QTRY_COMPARE(screenCast.closed, 2);
    }
    void portalClosureMatchesCurrentSession() {
        threeScreens();
        auto capture = backend();
        capture->startSharing();
        QTRY_VERIFY(capture->sharing());
        const auto previous = screenCast.sessions.last();
        capture->resetScreenSelection();
        capture->startSharing();
        QTRY_VERIFY(capture->sharing());
        const auto current = screenCast.sessions.last();
        QVERIFY(current != previous);
        auto signal = QDBusMessage::createSignal(previous, "org.freedesktop.portal.Session", "Closed");
        QVERIFY(QDBusConnection::sessionBus().send(signal));
        QTest::qWait(60);
        QVERIFY(capture->sharing());
        signal = QDBusMessage::createSignal(current, "org.freedesktop.portal.Session", "Closed");
        QVERIFY(QDBusConnection::sessionBus().send(signal));
        QTRY_VERIFY(!capture->sharing());
        QTRY_COMPARE(screenCast.closed, 2);
        QCOMPARE(control->stopped, 6);
    }
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
        QVERIFY(control->activeNodes.isEmpty());
        QCOMPARE(control->stopped, 0);
        QVERIFY(control->activeNodes.isEmpty());
        const auto first = qvariant_cast<CaptureFrame>(frames.first().first()).image;
        control->frame = image.copy();
        control->frame.fill(Qt::red);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(qvariant_cast<CaptureFrame>(frames.last().first()).image.pixelColor(10, 10), QColor(Qt::red));
        QVERIFY(qvariant_cast<CaptureFrame>(frames.last().first()).image != first);
        QVERIFY(control->activeNodes.isEmpty());
        QCOMPARE(screenCast.created, 1);
        capture->stopSharing();
        QTRY_COMPARE(screenCast.closed, 1);
        QCOMPARE(control->stopped, 1);
    }
#else
    void multipleScreensRequirePipeWire() {
        testLayout << CaptureScreen{"external", {800, 0, 800, 600}, {}};
        auto capture = backend();
        capture->setMethod(WaylandCapture::Method::ScreenCast);
        QSignalSpy failed(capture.get(), &ICaptureBackend::failed), frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region, 1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains("PipeWire"));
        QVERIFY(frames.isEmpty());
        QVERIFY(screenshot.requests.isEmpty());
    }
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
    void oneShotAllMonitors_data() {
        QTest::addColumn<int>("target");
        QTest::newRow("all-monitors") << -1;
        QTest::newRow("hdmi-only") << 1;
        QTest::newRow("displayport-only") << 2;
    }
    void oneShotAllMonitors() {
        QFETCH(int, target);
        permissionStore.records[""] = {"no"};
        screenshot.respectPermissions = true;
        testLayout = {{"eDP-1", {0, 0, 960, 600}, {}, 1},
                      {"HDMI-1", {-1280, 0, 1280, 720}, {}, 1.5},
                      {"DP-1", {960, -180, 800, 600}, {}, 2}};
        CaptureFrame atlas;
        atlas.desktop = {-1280, -180, 3040, 900};
        atlas.image = QImage(6080, 1800, QImage::Format_RGB32);
        atlas.image.fill(Qt::black);
        const QList<QColor> colors{Qt::green, Qt::blue, Qt::red};
        QPainter painter(&atlas.image);
        for (int i = 0; i < 3; ++i)
            painter.fillRect(atlas.pixelRect(testLayout[i].desktop), colors[i]);
        painter.end();
        QVERIFY(atlas.image.save(images.filePath("all-monitors.png")));
        screenshot.uri = QUrl::fromLocalFile(images.filePath("all-monitors.png")).toString();
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured), errors(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region, target);
        QTRY_COMPARE(frames.size(), 1);
        QVERIFY(errors.isEmpty());
        const auto frame = qvariant_cast<CaptureFrame>(frames.first().first());
        QCOMPARE(frame.screens.size(), target < 0 ? 3 : 1);
        for (const auto &screen : frame.screens) {
            int index = 0;
            while (testLayout[index].name != screen.name)
                ++index;
            QCOMPARE(screen.image.size(), (screen.desktop.size() * 2).toSize());
            QCOMPARE(screen.image.pixelColor(20, 20), colors[index]);
        }
        QCOMPARE(screenshot.requests, QList<bool>{false});
        QCOMPARE(screenCast.created, 0);
        QCOMPARE(screenCast.opened, 0);
        QVERIFY(!capture->sharing());
        QCOMPARE(permissionStore.records.value(""), QStringList{"no"});
        QCOMPARE(permissionStore.writes, 0);
        // 三块屏幕的视频不会导致重复 Screenshot 调用或后台流连接。
        QTest::qWait(80);
        QCOMPARE(screenshot.requests.size(), 1);
    }
    void oneShotDeniedDoesNotStartSharing() {
        screenshot.code = 2;
        auto capture = backend(false);
        QSignalSpy errors(capture.get(), &ICaptureBackend::failed), frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(screenCast.created, 0);
        QVERIFY(frames.isEmpty());
        screenshot.code = 0;
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QCOMPARE(screenshot.requests, (QList<bool>{false, false}));
    }
    void dedicatedIdentityAvoidsRememberedHostDenial() {
        permissionStore.records = {{"", {"no"}}, {"another.app", {"yes"}}};
        screenshot.respectPermissions = true;
        // 先复现旧实现：共享连接被缓存为空身份，收到与用户相同的 code=2、无 URI。
        bool denied = false;
        auto *oldRequest = new PortalRequest(this);
        oldRequest->call("org.freedesktop.portal.Screenshot", "Screenshot", {QString()},
                         {{"interactive", false}}, [&](uint code, QVariantMap data, QString) {
            QCOMPARE(code, uint(2));
            QVERIFY(!data.contains("uri"));
            denied = true;
        });
        QTRY_VERIFY(denied);
        const auto sharedSender = QDBusConnection::sessionBus().baseService();
        QVERIFY(registryState.identities.contains(sharedSender));
        QVERIFY(registryState.identities.value(sharedSender).isEmpty());
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured), errors(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QVERIFY(errors.isEmpty());
        QCOMPARE(registryState.registeredSenders.size(), 1);
        const auto registered = registryState.registeredSenders.first();
        QVERIFY(registered != sharedSender);
        QCOMPARE(registryState.screenshotSenders.last(), registered);
        QCOMPARE(registryState.identities.value(registered), QString("io.snapling.SnapLing"));
        const auto registerIndex = registryState.operations.indexOf("Register:" + registered);
        QVERIFY(registerIndex >= 0);
        QVERIFY(registryState.operations.indexOf("Screenshot:" + registered) > registerIndex);
        QCOMPARE(permissionStore.records.value(""), QStringList{"no"});
        QCOMPARE(permissionStore.records.value("another.app"), QStringList{"yes"});
        QVERIFY(!permissionStore.records.contains("io.snapling.SnapLing"));
        QCOMPARE(permissionStore.writes, 0);
        QCOMPARE(screenCast.created, 0);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(registryState.registeredSenders.size(), 1);
        QCOMPARE(registryState.screenshotSenders.last(), registered);
    }
    void identityRegistrationFailureDoesNotUseAnonymousScreenshot() {
        registryState.fail = true;
        registryState.cacheOnFailure = true;
        permissionStore.records[""] = {"no"};
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured), errors(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("无法注册"));
        QVERIFY(screenshot.requests.isEmpty());
        QVERIFY(frames.isEmpty());
        QCOMPARE(permissionStore.writes, 0);
        QCOMPARE(permissionStore.records.value(""), QStringList{"no"});
        registryState.fail = false;
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QCOMPARE(registryState.registeredSenders.size(), 1);
        QVERIFY(registryState.operations.first() != "Register:" + registryState.registeredSenders.first());
    }
    void cancelDuringIdentityRegistrationAndRetry() {
        registryState.delay = 100;
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(registryState.operations.size(), 1);
        capture->cancel();
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QTest::qWait(150);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(registryState.registeredSenders.size(), 1);
        QCOMPARE(screenshot.requests.size(), 1);
    }
    void backendDestroyedWhileRegisteringDoesNotCapture() {
        registryState.delay = 100;
        auto capture = backend(false);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(registryState.operations.size(), 1);
        capture.reset();
        QTest::qWait(150);
        QVERIFY(screenshot.requests.isEmpty());
    }
    void oldPortalWithoutRegistryKeepsLegacyCapture() {
        registryState.supported = false;
        permissionStore.records[""] = {"yes"};
        screenshot.respectPermissions = true;
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(registryState.operations.count("Register:" + registryState.screenshotSenders.first()), 1);
        QVERIFY(registryState.registeredSenders.isEmpty());
        QCOMPARE(permissionStore.writes, 0);
    }
    void portalRestartRequiresIdentityRegistrationAgain() {
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.unregisterService(portalService));
        registryState.identities.clear();
        QVERIFY(bus.registerService(portalService));
        QTest::qWait(50);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(registryState.registeredSenders.size(), 2);
        QCOMPARE(registryState.registeredSenders.first(), registryState.registeredSenders.last());
    }
    void screenshotPermissionDiagnostics_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("decision");
        QTest::addColumn<QString>("message");
        QTest::newRow("app-denied") << "io.snapling.SnapLing" << "no" << "已记住";
        QTest::newRow("unidentified-host-denied") << "" << "no" << "不用于本次请求";
        QTest::newRow("app-allowed-service-failure") << "io.snapling.SnapLing" << "yes" << "连接已注册";
    }
    void screenshotPermissionDiagnostics() {
        QFETCH(QString, id);
        QFETCH(QString, decision);
        QFETCH(QString, message);
        permissionStore.records[id] = {decision};
        screenshot.code = 2;
        auto capture = backend(false);
        QSignalSpy errors(capture.get(), &ICaptureBackend::failed);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY2(errors.first().first().toString().contains(message), qPrintable(errors.first().first().toString()));
        QCOMPARE(permissionStore.writes, 0);
        QCOMPARE(screenCast.created, 0);
    }
    void reRequestScreenshotPermissionFromMainWindow() {
        permissionStore.records = {{"io.snapling.SnapLing", {"no"}}, {"", {"no"}}, {"another.app", {"yes"}}};
        screenshot.respectPermissions = true;
        Application app(nullptr, backend(false));
        app.command({"--capture"});
        auto *main = windows<QMainWindow>().first();
        QTRY_VERIFY(main->findChild<QLabel *>("captureStatus")->text().contains("已记住"));
        auto *retry = main->findChild<QPushButton *>("retryScreenshotPermission");
        QVERIFY(retry);
        QVERIFY(retry->isVisible());
        retry->click();
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QCOMPARE(permissionStore.writes, 1);
        QCOMPARE(permissionStore.writtenApp, QString("io.snapling.SnapLing"));
        QVERIFY(permissionStore.writtenPermissions.isEmpty());
        QCOMPARE(permissionStore.records.value(""), QStringList{"no"});
        QCOMPARE(permissionStore.records.value("another.app"), QStringList{"yes"});
        QCOMPARE(screenshot.requests, (QList<bool>{false, false}));
        QCOMPARE(screenCast.created, 0);
    }
    void permissionResetFailureDoesNotCaptureOrGrant() {
        permissionStore.rejectWrite = true;
        auto capture = backend(false);
        QSignalSpy errors(capture.get(), &ICaptureBackend::failed), reset(capture.get(), &WaylandCapture::screenshotPermissionReset);
        capture->resetScreenshotPermission();
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(reset.isEmpty());
        QCOMPARE(permissionStore.writes, 0);
        QVERIFY(screenshot.requests.isEmpty());
    }
    void cancelPermissionResetIgnoresCompletionAndCanRetry() {
        auto capture = backend(false);
        QSignalSpy reset(capture.get(), &WaylandCapture::screenshotPermissionReset);
        capture->resetScreenshotPermission();
        capture->cancel();
        QTest::qWait(50);
        QVERIFY(reset.isEmpty());
        QVERIFY(screenshot.requests.isEmpty());
        capture->resetScreenshotPermission();
        QTRY_COMPARE(reset.size(), 1);
        QVERIFY(screenshot.requests.isEmpty());
    }
    void captureFailuresAreVisibleAndRetryable_data() {
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("badLayout");
        QTest::addColumn<QString>("message");
        QTest::newRow("denied-or-service-error") << 2 << false << "响应码 2";
        QTest::newRow("cancelled") << 1 << false << "响应码 1";
        QTest::newRow("wrong-layout") << 0 << true << "不匹配";
    }
    void captureFailuresAreVisibleAndRetryable() {
        QFETCH(int, code);
        QFETCH(bool, badLayout);
        QFETCH(QString, message);
        const auto originalLayout = testLayout;
        screenshot.code = uint(code);
        if (badLayout)
            testLayout << CaptureScreen{"external", {800, 0, 800, 600}, {}};
        Application app(nullptr, backend(false));
        auto *main = windows<QMainWindow>().first();
        main->showMinimized();
        app.command({"--capture"});
        auto *status = main->findChild<QLabel *>("captureStatus");
        QVERIFY(status);
        QTRY_VERIFY(status->text().contains(message));
        QVERIFY(main->isVisible());
        QVERIFY(!main->isMinimized());
        QVERIFY(status->isVisible());
        QVERIFY(!status->styleSheet().isEmpty());
        QVERIFY(windows<SelectionOverlay>().isEmpty());
        QCOMPARE(screenCast.created, 0);
        QCOMPARE(screenshot.requests.size(), 1);
        screenshot.code = 0;
        testLayout = originalLayout;
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QVERIFY(status->styleSheet().isEmpty());
        QCOMPARE(screenshot.requests.size(), 2);
    }
    void oneShotRejectsWrongDesktopSize() {
        testLayout << CaptureScreen{"external", {800, 0, 800, 600}, {}};
        auto capture = backend(false);
        QSignalSpy errors(capture.get(), &ICaptureBackend::failed), frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("不匹配"));
        QVERIFY(frames.isEmpty());
        QCOMPARE(screenCast.created, 0);
    }
    void cancelOneShotAndIgnoreLateResult() {
        screenshot.delay = 100;
        auto capture = backend(false);
        QSignalSpy frames(capture.get(), &ICaptureBackend::captured);
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(screenshot.requests.size(), 1);
        capture->cancel();
        screenshot.delay = 0;
        capture->capture(CaptureMode::Region);
        QTRY_COMPARE(frames.size(), 1);
        QTest::qWait(150);
        QCOMPARE(frames.size(), 1);
    }
    void clickSelectsMonitorAndDragSelectsRegion() {
        for (bool drag : {false, true}) {
            Application app(nullptr, backend(false));
            app.command({"--capture"});
            QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
            auto *overlay = windows<SelectionOverlay>().first();
            QEnterEvent enter({50, 50}, {50, 50}, {50, 50});
            QApplication::sendEvent(overlay, &enter);
            QCOMPARE(overlay->selection().size(), QSizeF(image.size()));
            QVERIFY(!overlay->canvasWidget());
            QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, {50, 50});
            QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, drag ? QPoint(200, 170) : QPoint(50, 50));
            QTRY_COMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
            QCOMPARE(overlay->session()->document.original().size(), drag ? QSize(150, 120) : image.size());
            QVERIFY(overlay->findChild<QFrame *>("quickToolbar")->isVisible());
            QVERIFY(windows<EditorWindow>().isEmpty());
            overlay->close();
            QTRY_VERIFY(windows<SelectionOverlay>().isEmpty());
        }
        QCOMPARE(screenCast.created, 0);
    }
    void waylandOutputsExposeFrozenOverlays() {
        if (!QGuiApplication::platformName().contains("wayland"))
            QSKIP("需要真实 Wayland compositor；常规 D-Bus 测试使用 minimal QPA");
        QVERIFY(testLayout.size() >= 3);
        CaptureFrame atlas;
        for (const auto &screen : testLayout)
            atlas.desktop = atlas.desktop.united(screen.desktop);
        atlas.image = QImage(atlas.desktop.size().toSize(), QImage::Format_RGB32);
        atlas.image.fill(Qt::darkGreen);
        QVERIFY(atlas.image.save(images.filePath("wayland.png")));
        screenshot.uri = QUrl::fromLocalFile(images.filePath("wayland.png")).toString();
        Application app(nullptr, backend(false));
        app.command({"--capture"});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), testLayout.size());
        // 合成器可能先发送默认输出 enter，然后才 leave/enter 到全屏指定输出。
        QTest::qWait(200);
        // 可选：通过 Weston 的 X11 后端注入鼠标，走实际 Wayland seat，避免直接给 QWidget 发事件。
        const auto nativeWindows = qEnvironmentVariable("SNAPLING_TEST_WAYLAND_X11_WINDOWS").split(',', Qt::SkipEmptyParts);
        const bool nativeMouse = !nativeWindows.isEmpty();
        if (nativeMouse)
            QCOMPARE(nativeWindows.size(), testLayout.size());
        auto native = [](QStringList arguments) {
            QProcess process;
            process.start("xdotool", arguments);
            return process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };
        QSet<QString> exposed;
        for (auto *overlay : windows<SelectionOverlay>()) {
            QTRY_VERIFY(overlay->windowHandle()->isExposed());
            int screenIndex = 0;
            while (testLayout[screenIndex].name != overlay->captureScreenName())
                ++screenIndex;
            QCOMPARE(overlay->size(), testLayout[screenIndex].desktop.size().toSize());
            exposed.insert(overlay->captureScreenName());
            if (nativeMouse) {
                QVERIFY(native({"windowraise", nativeWindows[screenIndex]}));
                QVERIFY(native({"mousemove", "--window", nativeWindows[screenIndex], "80", "80"}));
            } else {
                QEnterEvent enter({80, 80}, {80, 80}, {80, 80});
                QApplication::sendEvent(overlay, &enter);
            }
            QTRY_COMPARE(overlay->selection().size(), QSizeF(overlay->size()));
            if (nativeMouse)
                for (auto *other : windows<SelectionOverlay>())
                    if (other != overlay)
                        QTRY_VERIFY(other->selection().isEmpty());
            const auto preview = overlay->grab().toImage();
            QCOMPARE(preview.pixelColor(preview.width() / 2, 0), QColor("#0bd18b"));
            QVERIFY(preview.save(QCoreApplication::applicationDirPath() + "/wayland-" + overlay->captureScreenName() + ".png"));
            if (!nativeMouse) {
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(overlay, &leave);
                QVERIFY(overlay->selection().isEmpty());
            }
        }
        QCOMPARE(exposed.size(), testLayout.size());
        QTest::qWait(250);
        SelectionOverlay *overlay = nullptr;
        for (auto *candidate : windows<SelectionOverlay>()) {
            QVERIFY(candidate->windowHandle()->isExposed());
            // Wayland 不提供全局窗口位置；Qt 的推算 screen() 可能变化。
            // 原生路径以对应输出中的鼠标命中、边框和导出尺寸验收。
            if (candidate->captureScreenName() == testLayout.last().name)
                overlay = candidate;
        }
        QVERIFY(overlay);
        if (nativeMouse) {
            QVERIFY(native({"windowraise", nativeWindows.last()}));
            QVERIFY(native({"mousemove", "--window", nativeWindows.last(), "80", "80"}));
            QTRY_VERIFY(!overlay->selection().isEmpty());
            QVERIFY(native({"click", "1"}));
        } else
            QTest::mouseClick(overlay, Qt::LeftButton, Qt::NoModifier, {80, 80});
        QTRY_COMPARE(windows<SelectionOverlay>().size(), 1);
        QTRY_COMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
        QVERIFY(overlay->findChild<QFrame *>("quickToolbar")->isVisible());
        QCOMPARE(overlay->session()->document.original().size(), atlas.pixelRect(testLayout.last().desktop).size());
        if (nativeMouse) {
            qInfo("native_wayland_monitor_hover_and_click=PASS");
            overlay->close();
            QTRY_VERIFY(windows<SelectionOverlay>().isEmpty());
            app.command({"--capture"});
            QTRY_COMPARE(windows<SelectionOverlay>().size(), testLayout.size());
            QTest::qWait(200);
            for (auto *candidate : windows<SelectionOverlay>())
                if (candidate->captureScreenName() == testLayout.last().name)
                    overlay = candidate;
            QTRY_VERIFY(overlay->windowHandle()->isExposed());
            QVERIFY(native({"windowraise", nativeWindows.last()}));
            QVERIFY(native({"mousemove", "--window", nativeWindows.last(), "80", "80"}));
            QTest::qWait(40);
            QVERIFY(native({"mousedown", "1"}));
            QTest::qWait(40);
            QVERIFY(native({"mousemove", "--window", nativeWindows.last(), "300", "250"}));
            QTRY_COMPARE(overlay->selection().size(), QSizeF(220, 170));
            QVERIFY(native({"mouseup", "1"}));
            QTRY_COMPARE(overlay->phase(), SelectionOverlay::Phase::QuickEditing);
            QCOMPARE(overlay->session()->document.original().size(), QSize(220, 170));
            QVERIFY(overlay->findChild<QFrame *>("quickToolbar")->isVisible());
            qInfo("native_wayland_external_monitor_drag=PASS");
        }
        QCOMPARE(screenCast.created, 0);
    }
    void cleanupTestCase() {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(portalRoot);
        bus.unregisterService(portalService);
        bus.unregisterObject("/org/freedesktop/impl/portal/PermissionStore");
        bus.unregisterService("org.freedesktop.impl.portal.PermissionStore");
    }
};
int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("XDG_CACHE_HOME", config.path().toUtf8());
    if (!qEnvironmentVariableIsSet("SNAPLING_TEST_WAYLAND"))
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

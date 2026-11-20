#include "hotkey/Hotkey.h"
#include "platform/linux/wayland/Portal.h"
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace SnapLing;
static const QString root = "/org/freedesktop/portal/desktop";
static const QString service = "org.freedesktop.portal.Desktop";
static const QString iface = "org.freedesktop.portal.GlobalShortcuts";
struct TestShortcut { QString id; QVariantMap properties; };
using TestShortcuts = QList<TestShortcut>;
Q_DECLARE_METATYPE(TestShortcut)
Q_DECLARE_METATYPE(TestShortcuts)
QDBusArgument &operator<<(QDBusArgument &a, const TestShortcut &s) {
    a.beginStructure(); a << s.id << s.properties; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, TestShortcut &s) {
    a.beginStructure(); a >> s.id >> s.properties; a.endStructure(); return a;
}
class HotkeySession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
  public:
    explicit HotkeySession(QObject *parent) : QObject(parent) {}
    int closes = 0;
  public slots:
    void Close() { ++closes; }
};
class HotkeyPortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
    Q_PROPERTY(uint version READ version CONSTANT)
  public:
    uint version() const { return 1; }
    QStringList sessions;
    TestShortcuts shortcuts;
    QList<HotkeySession *> objects;
    uint bindCode = 0;
    int bound = 0;
    QDBusObjectPath response(const QVariantMap &options, uint code, QVariantMap data) {
        const auto sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
        const auto path = root + "/request/" + sender + "/" + options["handle_token"].toString();
        QTimer::singleShot(0, this, [path, code, data] {
            auto message = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            message.setArguments({code, data});
            QDBusConnection::sessionBus().send(message);
        });
        return QDBusObjectPath(path);
    }
    void activate(QString session, QString token = {}, QString id = "capture") {
        auto message = QDBusMessage::createSignal(root, iface, "Activated");
        message.setArguments({QVariant::fromValue(QDBusObjectPath(session)), id,
                              QVariant::fromValue(qulonglong(10)), QVariantMap{{"activation_token", token}}});
        QDBusConnection::sessionBus().send(message);
    }
  public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options) {
        const auto path = root + "/session/" + options["session_handle_token"].toString();
        auto *object = new HotkeySession(this);
        QDBusConnection::sessionBus().registerObject(path, object, QDBusConnection::ExportAllSlots);
        objects << object;
        sessions << path;
        return response(options, 0, {{"session_handle", QVariant::fromValue(QDBusObjectPath(path))}});
    }
    QDBusObjectPath BindShortcuts(const QDBusObjectPath &session, const TestShortcuts &requested,
                                 const QString &, const QVariantMap &options) {
        if (!sessions.contains(session.path()))
            return response(options, 2, {});
        ++bound;
        shortcuts = requested;
        TestShortcuts result = requested;
        for (auto &shortcut : result)
            shortcut.properties["trigger_description"] = "Ctrl+Alt+A";
        return response(options, bindCode, {{"shortcuts", QVariant::fromValue(result)}});
    }
    QDBusObjectPath Stall(const QVariantMap &options) {
        return QDBusObjectPath(root + "/request/stalled/" + options["handle_token"].toString());
    }
};
class TestPortalHotkey : public QObject {
    Q_OBJECT
    HotkeyPortal portal;
  private slots:
    void initTestCase() {
        qDBusRegisterMetaType<TestShortcut>();
        qDBusRegisterMetaType<TestShortcuts>();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(service));
        QVERIFY(bus.registerObject(root, &portal, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    }
    void init() { portal.bindCode = 0; portal.bound = 0; }
    void nativeSessionAndActivationToken() {
        auto hotkey = createHotkeyBackend();
        QSignalSpy statuses(hotkey.get(), &IHotkeyBackend::status);
        QSignalSpy activated(hotkey.get(), &IHotkeyBackend::activated);
        QSignalSpy tokens(hotkey.get(), &IHotkeyBackend::activationTokenAvailable);
        hotkey->bind(QKeySequence("Ctrl+Alt+A"));
        QTRY_VERIFY(!statuses.isEmpty() && statuses.last()[1].toBool());
        QCOMPARE(portal.bound, 1);
        QCOMPARE(portal.shortcuts.first().properties["preferred_trigger"].toString(), QString("CTRL+ALT+a"));
        portal.activate(portal.sessions.last(), "test-token");
        QTRY_COMPARE(activated.size(), 1);
        QCOMPARE(tokens.first().first().toString(), QString("test-token"));
        portal.activate(portal.sessions.last(), {}, "other-id");
        QTest::qWait(30);
        QCOMPARE(activated.size(), 1);
        hotkey->unbind();
        QTRY_COMPARE(portal.objects.last()->closes, 1);
    }
    void rebindIgnoresOldSession() {
        auto hotkey = createHotkeyBackend();
        QSignalSpy statuses(hotkey.get(), &IHotkeyBackend::status), activated(hotkey.get(), &IHotkeyBackend::activated);
        hotkey->bind(QKeySequence("Ctrl+Alt+A"));
        QTRY_VERIFY(statuses.last()[1].toBool());
        const auto previous = portal.sessions.last();
        hotkey->bind(QKeySequence("Ctrl+Shift+Print"));
        QTRY_COMPARE(portal.bound, 2);
        QTRY_VERIFY(statuses.last()[1].toBool());
        QCOMPARE(portal.shortcuts.first().properties["preferred_trigger"].toString(), QString("CTRL+SHIFT+Print"));
        portal.activate(previous);
        portal.activate(portal.sessions.last());
        QTRY_COMPARE(activated.size(), 1);
        QTest::qWait(40);
        QCOMPARE(activated.size(), 1);
    }
    void deniedAndEmptyDoNotActivate() {
        auto hotkey = createHotkeyBackend();
        QSignalSpy statuses(hotkey.get(), &IHotkeyBackend::status), activated(hotkey.get(), &IHotkeyBackend::activated);
        portal.bindCode = 1;
        hotkey->bind(QKeySequence("Ctrl+Alt+A"));
        QTRY_VERIFY(!statuses.isEmpty() && statuses.last()[0].toString().contains("取消或拒绝"));
        QVERIFY(!statuses.last()[1].toBool());
        portal.activate(portal.sessions.last());
        QTest::qWait(30);
        QVERIFY(activated.isEmpty());
        const auto count = portal.sessions.size();
        hotkey->bind({});
        QTest::qWait(30);
        QCOMPARE(portal.sessions.size(), count);
    }
    void closedSessionStopsActivation() {
        auto hotkey = createHotkeyBackend();
        QSignalSpy statuses(hotkey.get(), &IHotkeyBackend::status), activated(hotkey.get(), &IHotkeyBackend::activated);
        hotkey->bind(QKeySequence("Ctrl+Alt+A"));
        QTRY_VERIFY(statuses.last()[1].toBool());
        const auto path = portal.sessions.last();
        auto message = QDBusMessage::createSignal(path, "org.freedesktop.portal.Session", "Closed");
        QDBusConnection::sessionBus().send(message);
        QTRY_VERIFY(!statuses.last()[1].toBool());
        portal.activate(path);
        QTest::qWait(30);
        QVERIFY(activated.isEmpty());
    }
    void requestTimeoutAndCancellation() {
        bool finished = false;
        auto *request = new PortalRequest(this, 30);
        request->call(iface, "Stall", {}, {}, [&](uint code, QVariantMap, QString error) {
            QCOMPARE(code, uint(2));
            QVERIFY(error.contains("超时"));
            finished = true;
        });
        QTRY_VERIFY(finished);
        finished = false;
        request = new PortalRequest(this);
        request->call(iface, "Stall", {}, {}, [&](uint code, QVariantMap, QString error) {
            QCOMPARE(code, uint(1));
            QVERIFY(error.contains("取消"));
            finished = true;
        });
        request->cancel();
        QVERIFY(finished);
    }
    void cleanupTestCase() {
        auto bus = QDBusConnection::sessionBus();
        for (const auto &path : portal.sessions)
            bus.unregisterObject(path);
        bus.unregisterObject(root);
        bus.unregisterService(service);
    }
};
int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "minimal");
    qputenv("XDG_SESSION_TYPE", "wayland");
    qunsetenv("SNAPLING_TEST_X11");
    QApplication app(argc, argv);
    TestPortalHotkey test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestPortalHotkey.moc"

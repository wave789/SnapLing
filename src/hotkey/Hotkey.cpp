#include "Hotkey.h"
#ifndef Q_OS_WIN
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#endif
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QSocketNotifier>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include "platform/linux/wayland/Portal.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QUuid>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif
namespace SnapLing {
#ifdef Q_OS_WIN
class NativeHotkey final : public IHotkeyBackend, public QAbstractNativeEventFilter {
    inline static int nextId = 0x534c;
    const int id = nextId++;

  public:
    NativeHotkey() {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
    ~NativeHotkey() override {
        unbind();
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    void unbind() override {
        UnregisterHotKey(nullptr, id);
    }
    void bind(const QKeySequence &key) override {
        unbind();
        if (key.isEmpty())
            return;
        auto k = key[0];
        UINT mods = MOD_NOREPEAT;
        if (k.keyboardModifiers() & Qt::ControlModifier)
            mods |= MOD_CONTROL;
        if (k.keyboardModifiers() & Qt::AltModifier)
            mods |= MOD_ALT;
        if (k.keyboardModifiers() & Qt::ShiftModifier)
            mods |= MOD_SHIFT;
        if (k.keyboardModifiers() & Qt::MetaModifier)
            mods |= MOD_WIN;
        int v = int(k.key());
        if (v >= Qt::Key_F1 && v <= Qt::Key_F24)
            v = VK_F1 + v - Qt::Key_F1;
        if (v == Qt::Key_Print)
            v = VK_SNAPSHOT;
        bool ok = RegisterHotKey(nullptr, id, mods, UINT(v));
        emit status(ok ? "全局快捷键已注册" : "快捷键已占用或不受支持，请在设置中更换", ok);
    }
    bool nativeEventFilter(const QByteArray &, void *m, qintptr *) override {
        auto *msg = static_cast<MSG *>(m);
        if (msg->message == WM_HOTKEY && msg->wParam == WPARAM(id)) {
            emit activated();
            return true;
        }
        return false;
    }
};
#else
class XHotkey final : public IHotkeyBackend {
    Display *display = nullptr;
    std::unique_ptr<QSocketNotifier> notifier;
    int code = 0;
    unsigned mods = 0;
    inline static bool error = false;
    static int handler(Display *, XErrorEvent *) {
        error = true;
        return 0;
    }

  public:
    XHotkey() {
        display = XOpenDisplay(nullptr);
        if (display) {
            notifier = std::make_unique<QSocketNotifier>(ConnectionNumber(display), QSocketNotifier::Read);
            connect(notifier.get(), &QSocketNotifier::activated, this, [this] {
                while (XPending(display)) {
                    XEvent e{};
                    XNextEvent(display, &e);
                    if (e.type == KeyPress && e.xkey.keycode == unsigned(code))
                        emit activated();
                }
            });
        }
    }
    ~XHotkey() override {
        unbind();
        notifier.reset();
        if (display)
            XCloseDisplay(display);
    }
    void unbind() override {
        if (display && code) {
            XUngrabKey(display, code, AnyModifier, DefaultRootWindow(display));
            XSync(display, False);
        }
        code = 0;
    }
    void bind(const QKeySequence &key) override {
        unbind();
        if (!display || key.isEmpty()) {
            emit status("无法注册 X11 快捷键", false);
            return;
        }
        auto combo = key[0];
        int k = int(combo.key());
        KeySym sym = NoSymbol;
        if (k >= Qt::Key_F1 && k <= Qt::Key_F35)
            sym = XK_F1 + k - Qt::Key_F1;
        else if (k == Qt::Key_Print)
            sym = XK_Print;
        else {
            QByteArray s = QString(QChar(k)).toLower().toLatin1();
            sym = XStringToKeysym(s.constData());
        }
        code = XKeysymToKeycode(display, sym);
        mods = 0;
        if (combo.keyboardModifiers() & Qt::ControlModifier)
            mods |= ControlMask;
        if (combo.keyboardModifiers() & Qt::AltModifier)
            mods |= Mod1Mask;
        if (combo.keyboardModifiers() & Qt::ShiftModifier)
            mods |= ShiftMask;
        if (combo.keyboardModifiers() & Qt::MetaModifier)
            mods |= Mod4Mask;
        if (!code) {
            emit status("不支持此快捷键", false);
            return;
        }
        error = false;
        auto previous = XSetErrorHandler(handler);
        for (unsigned extra : {0u, unsigned(LockMask), unsigned(Mod2Mask), unsigned(LockMask | Mod2Mask)})
            XGrabKey(display, code, mods | extra, DefaultRootWindow(display), False, GrabModeAsync,
                     GrabModeAsync);
        XSync(display, False);
        XSetErrorHandler(previous);
        if (error) {
            unbind();
            emit status("全局快捷键冲突，请更换快捷键", false);
        } else
            emit status("全局快捷键已注册", true);
    }
};
struct Shortcut {
    QString id;
    QVariantMap properties;
};
const QDBusArgument &operator>>(const QDBusArgument &a, Shortcut &s) {
    a.beginStructure();
    a >> s.id >> s.properties;
    a.endStructure();
    return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const Shortcut &s) {
    a.beginStructure();
    a << s.id << s.properties;
    a.endStructure();
    return a;
}
}
Q_DECLARE_METATYPE(SnapLing::Shortcut)
Q_DECLARE_METATYPE(QList<SnapLing::Shortcut>)
namespace SnapLing {
class PortalHotkey final : public IHotkeyBackend {
    Q_OBJECT
    QString session;
    QPointer<PortalRequest> request;
    quint64 generation = 0;
    bool registered = false;

    void fail(QString message) {
        unbind();
        emit status(message + "。可在 GNOME 自定义快捷键中绑定截图命令，或点击“重新注册快捷键”。", false);
    }
    static QString trigger(const QKeySequence &key) {
        if (key.count() != 1)
            return {};
        const auto combination = key[0];
        const int code = int(combination.key());
        QString symbol;
        if (code >= Qt::Key_A && code <= Qt::Key_Z)
            symbol = QChar('a' + code - Qt::Key_A);
        else if (code >= Qt::Key_0 && code <= Qt::Key_9)
            symbol = QChar('0' + code - Qt::Key_0);
        else if (code >= Qt::Key_F1 && code <= Qt::Key_F35)
            symbol = "F" + QString::number(code - Qt::Key_F1 + 1);
        else {
            switch (code) {
            case Qt::Key_Print: symbol = "Print"; break;
            case Qt::Key_Return: symbol = "Return"; break;
            case Qt::Key_Enter: symbol = "KP_Enter"; break;
            case Qt::Key_Escape: symbol = "Escape"; break;
            case Qt::Key_Space: symbol = "space"; break;
            case Qt::Key_Tab: symbol = "Tab"; break;
            case Qt::Key_Backspace: symbol = "BackSpace"; break;
            case Qt::Key_Delete: symbol = "Delete"; break;
            case Qt::Key_Insert: symbol = "Insert"; break;
            case Qt::Key_Home: symbol = "Home"; break;
            case Qt::Key_End: symbol = "End"; break;
            case Qt::Key_PageUp: symbol = "Prior"; break;
            case Qt::Key_PageDown: symbol = "Next"; break;
            case Qt::Key_Left: symbol = "Left"; break;
            case Qt::Key_Right: symbol = "Right"; break;
            case Qt::Key_Up: symbol = "Up"; break;
            case Qt::Key_Down: symbol = "Down"; break;
            case Qt::Key_Plus: symbol = "plus"; break;
            case Qt::Key_Minus: symbol = "minus"; break;
            default: return {};
            }
        }
        QStringList pieces;
        const auto modifiers = combination.keyboardModifiers();
        if (modifiers & Qt::ControlModifier) pieces << "CTRL";
        if (modifiers & Qt::AltModifier) pieces << "ALT";
        if (modifiers & Qt::ShiftModifier) pieces << "SHIFT";
        if (modifiers & Qt::MetaModifier) pieces << "LOGO";
        pieces << symbol;
        return pieces.join('+');
    }

  public:
    PortalHotkey() {
        qDBusRegisterMetaType<Shortcut>();
        qDBusRegisterMetaType<QList<Shortcut>>();
        QDBusConnection::sessionBus().connect(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
            SLOT(triggered(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    }
    ~PortalHotkey() override { unbind(); }
    void unbind() override {
        ++generation;
        registered = false;
        if (request) {
            auto pending = request;
            request = nullptr;
            pending->cancel();
        }
        if (session.isEmpty())
            return;
        QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", session,
            "org.freedesktop.portal.Session", "Closed", this, SLOT(closed(QDBusMessage)));
        auto message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", session,
                                                      "org.freedesktop.portal.Session", "Close");
        session.clear();
        QDBusConnection::sessionBus().asyncCall(message);
    }
    void bind(const QKeySequence &key) override {
        unbind();
        if (key.isEmpty()) {
            emit status("未启用截图快捷键", false);
            return;
        }
        const auto preferred = trigger(key);
        if (preferred.isEmpty()) {
            emit status("此快捷键不受支持，请使用单组按键组合", false);
            return;
        }
        emit status("正在向系统注册快捷键… 请完成系统授权", false);
        const auto job = generation;
        PortalRequest::queryVersion("org.freedesktop.portal.GlobalShortcuts", this,
            [this, preferred, job](uint version, QString error) {
                if (job != generation)
                    return;
                if (!version) {
                    fail(error.isEmpty() ? "系统未提供全局快捷键 Portal" : error);
                    return;
                }
                request = new PortalRequest(this, 10000);
                request->call("org.freedesktop.portal.GlobalShortcuts", "CreateSession", {},
                    {{"session_handle_token", "snapling_" + QUuid::createUuid().toString(QUuid::Id128)}},
                    [this, preferred, job](uint code, QVariantMap data, QString error) {
                        if (job != generation)
                            return;
                        request = nullptr;
                        if (code) {
                            fail(error.isEmpty() ? "快捷键会话创建被取消或拒绝" : error);
                            return;
                        }
                        const auto handle = data.value("session_handle");
                        session = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                                      ? qvariant_cast<QDBusObjectPath>(handle).path() : handle.toString();
                        if (session.isEmpty() || session == "/" || !session.startsWith('/')) {
                            fail("系统未返回有效的快捷键会话");
                            return;
                        }
                        QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop", session,
                            "org.freedesktop.portal.Session", "Closed", this, SLOT(closed(QDBusMessage)));
                        const QList<Shortcut> shortcuts{
                            {"capture", {{"description", "SnapLing 截图"}, {"preferred_trigger", preferred}}}};
                        request = new PortalRequest(this);
                        request->call("org.freedesktop.portal.GlobalShortcuts", "BindShortcuts",
                            {QVariant::fromValue(QDBusObjectPath(session)), QVariant::fromValue(shortcuts), QString()}, {},
                            [this, job](uint code, QVariantMap data, QString error) {
                                if (job != generation)
                                    return;
                                request = nullptr;
                                if (code) {
                                    fail(error.isEmpty() ? "快捷键授权被取消或拒绝" : error);
                                    return;
                                }
                                const auto argument = qvariant_cast<QDBusArgument>(data.value("shortcuts"));
                                if (argument.currentSignature() != "a(sa{sv})") {
                                    fail("系统未返回有效的快捷键绑定");
                                    return;
                                }
                                const auto shortcuts = qdbus_cast<QList<Shortcut>>(argument);
                                for (const auto &shortcut : shortcuts)
                                    if (shortcut.id == "capture") {
                                        registered = true;
                                        const auto description = shortcut.properties.value("trigger_description").toString();
                                        emit status("系统已绑定截图快捷键" +
                                                    (description.isEmpty() ? QString() : "：" + description), true);
                                        return;
                                    }
                                fail("系统未绑定截图快捷键");
                            });
                    });
            });
    }

  private slots:
    void triggered(const QDBusObjectPath &path, const QString &id, qulonglong, const QVariantMap &options) {
        if (registered && path.path() == session && id == "capture") {
            emit activationTokenAvailable(options.value("activation_token").toString());
            emit activated();
        }
    }
    void closed(const QDBusMessage &message) {
        if (message.path() == session) {
            session.clear();
            fail("系统关闭了快捷键会话");
        }
    }
};
#endif
std::unique_ptr<IHotkeyBackend> createHotkeyBackend() {
#ifdef Q_OS_WIN
    return std::make_unique<NativeHotkey>();
#else
    if (QGuiApplication::platformName().contains("wayland") ||
        (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" &&
         !qEnvironmentVariableIsSet("SNAPLING_TEST_X11")))
        return std::make_unique<PortalHotkey>();
    return std::make_unique<XHotkey>();
#endif
}
} // namespace SnapLing
#ifndef Q_OS_WIN
#include "Hotkey.moc"
#endif

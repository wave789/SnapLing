#include "Hotkey.h"
#ifndef Q_OS_WIN
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#endif
#include <QCoreApplication>
#include <QGuiApplication>
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

  public:
    PortalHotkey() {
        qDBusRegisterMetaType<Shortcut>();
        qDBusRegisterMetaType<QList<Shortcut>>();
        QDBusConnection::sessionBus().connect(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
            SLOT(triggered(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    }
    ~PortalHotkey() override {
        unbind();
    }
    void unbind() override {
        if (session.isEmpty())
            return;
        auto m = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", session,
                                                "org.freedesktop.portal.Session", "Close");
        QDBusConnection::sessionBus().asyncCall(m);
        session.clear();
    }
    void bind(const QKeySequence &key) override {
        unbind();
        if (!PortalRequest::version("org.freedesktop.portal.GlobalShortcuts")) {
            emit status(
                "系统未提供全局快捷键 Portal。请在 GNOME 设置 → 键盘 → 自定义快捷键中绑定 snapling --capture",
                false);
            return;
        }
        auto *r = new PortalRequest(this);
        r->call("org.freedesktop.portal.GlobalShortcuts", "CreateSession", {},
                {{"session_handle_token", "snapling_" + QUuid::createUuid().toString(QUuid::Id128)}},
                [this, key](uint code, QVariantMap data, QString error) {
                    if (code) {
                        emit status(error.isEmpty() ? "快捷键授权取消，可绑定 snapling --capture" : error,
                                    false);
                        return;
                    }
                    session = data["session_handle"].toString();
                    QString trigger = key.toString(QKeySequence::PortableText)
                                          .replace("Ctrl+", "CTRL+")
                                          .replace("Alt+", "ALT+")
                                          .replace("Shift+", "SHIFT+")
                                          .replace("Meta+", "LOGO+");
                    QList<Shortcut> shortcuts{
                        {"capture", {{"description", "SnapLing 截图"}, {"preferred_trigger", trigger}}}};
                    auto *bindRequest = new PortalRequest(this);
                    bindRequest->call(
                        "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts",
                        {QVariant::fromValue(QDBusObjectPath(session)), QVariant::fromValue(shortcuts),
                         QString()},
                        {}, [this](uint c, QVariantMap data, QString error) {
                            auto shortcuts = qdbus_cast<QList<Shortcut>>(data["shortcuts"]);
                            bool ok = c == 0 && !shortcuts.isEmpty();
                            emit status(ok ? "系统已授权截图快捷键"
                                           : (error.isEmpty()
                                                  ? "未绑定快捷键；请配置系统自定义快捷键 snapling --capture"
                                                  : error),
                                        ok);
                        });
                });
    }
  private slots:
    void triggered(const QDBusObjectPath &s, const QString &id, qulonglong, const QVariantMap &) {
        if (s.path() == session && id == "capture")
            emit activated();
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

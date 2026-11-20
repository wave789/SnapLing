#include "PortalConnection.h"
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QTimer>
#include <QUuid>

namespace SnapLing {
static const QString service = "org.freedesktop.portal.Desktop";
PortalConnection::PortalConnection(QString id, QObject *parent)
    : QObject(parent), appId(std::move(id)),
      connectionName("snapling-screenshot-" + QUuid::createUuid().toString(QUuid::Id128)),
      connection(QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName)) {
    serviceWatcher = new QDBusServiceWatcher(service, connection, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &oldOwner, const QString &) {
        // 初次自动启动 Portal 是正常的；已有服务消失或替换后，旧注册不再有效。
        if (oldOwner.isEmpty())
            return;
        ++generation;
        const QPointer<PortalConnection> guard(this);
        complete(State::Unregistered, "系统截图服务已重启，请重新截图。");
        if (guard)
            emit serviceRestarted();
    });
}
PortalConnection::~PortalConnection() {
    QDBusConnection::disconnectFromBus(connectionName);
}
void PortalConnection::complete(State next, QString error) {
    state = next;
    const auto pending = std::move(waiters);
    waiters.clear();
    for (const auto &waiter : pending)
        if (waiter.context)
            waiter.callback(error);
}
void PortalConnection::ensureRegistered(QObject *context, Callback callback) {
    Q_ASSERT(context);
    if (!context)
        return;
    if (renewBeforeRegistration) {
        // 超时或错误回复并不保证服务端未缓存身份；重试必须换干净的唯一连接名。
        const auto previousName = connectionName;
        connectionName = "snapling-screenshot-" + QUuid::createUuid().toString(QUuid::Id128);
        connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
        serviceWatcher->setConnection(connection);
        QDBusConnection::disconnectFromBus(previousName);
        renewBeforeRegistration = false;
    }
    if (!connection.isConnected()) {
        QTimer::singleShot(0, context, [callback = std::move(callback)] {
            callback("无法连接用户 D-Bus 会话，请重新启动 SnapLing。");
        });
        return;
    }
    if (state == State::Registered || state == State::Legacy) {
        QTimer::singleShot(0, context, [callback = std::move(callback)] { callback({}); });
        return;
    }
    waiters << Waiter{context, std::move(callback)};
    if (state == State::Registering)
        return;
    state = State::Registering;
    const auto job = ++generation;
    // 不使用 QDBusInterface，避免先行 introspection 或其他 Portal 调用固定错误的身份。
    auto message = QDBusMessage::createMethodCall(service, "/org/freedesktop/portal/desktop",
                                                 "org.freedesktop.host.portal.Registry", "Register");
    message.setArguments({appId, QVariantMap{}});
    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, job] {
        const auto reply = watcher->reply();
        watcher->deleteLater();
        if (job != generation)
            return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            const auto name = reply.errorName();
            if (name == "org.freedesktop.DBus.Error.UnknownInterface" ||
                name == "org.freedesktop.DBus.Error.UnknownMethod") {
                // 旧 Portal 只能依靠系统启动器/沙箱识别身份，不伪称注册成功。
                qWarning("portal_identity=legacy_unverified reason=registry_unavailable");
                complete(State::Legacy);
                return;
            }
            qWarning("portal_identity=failed app_id=%s error=%s", qPrintable(appId), qPrintable(name));
            renewBeforeRegistration = true;
            complete(State::Unregistered,
                     "无法注册 SnapLing 的系统截图身份：" + name + "。" + reply.errorMessage().left(300) +
                     " 请确认已安装 io.snapling.SnapLing.desktop，再重试截图。");
            return;
        }
        qInfo("portal_identity=registered app_id=%s connection=dedicated", qPrintable(appId));
        complete(State::Registered);
    });
}
} // namespace SnapLing

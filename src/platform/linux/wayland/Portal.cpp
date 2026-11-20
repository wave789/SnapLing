#include "Portal.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QUuid>
namespace SnapLing {
static const QString service = "org.freedesktop.portal.Desktop", root = "/org/freedesktop/portal/desktop";
PortalRequest::PortalRequest(QObject *parent, int responseTimeoutMs)
    : PortalRequest(QDBusConnection::sessionBus(), parent, responseTimeoutMs) {}
PortalRequest::PortalRequest(const QDBusConnection &bus, QObject *parent, int responseTimeoutMs)
    : QObject(parent), connection(bus) {
    timer.setSingleShot(true);
    timer.setInterval(qMax(1, responseTimeoutMs));
    connect(&timer, &QTimer::timeout, this, [this] {
        if (!path.isEmpty()) {
            auto message = QDBusMessage::createMethodCall(service, path, "org.freedesktop.portal.Request", "Close");
            connection.asyncCall(message);
        }
        finish(2, {}, "系统 Portal 响应超时，请检查系统授权窗口或重新尝试。");
    });
}
void PortalRequest::queryVersion(const QString &iface, QObject *context,
                                 std::function<void(uint, QString)> callback) {
    auto message = QDBusMessage::createMethodCall(service, root, "org.freedesktop.DBus.Properties", "Get");
    message.setArguments({iface, QString("version")});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 2500), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [watcher, callback = std::move(callback)] {
        QDBusPendingReply<QDBusVariant> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError())
            callback(0, "Portal 接口探测失败：" + reply.error().name());
        else
            callback(reply.value().variant().toUInt(), {});
    });
}
uint PortalRequest::version(const QString &iface) {
    auto message = QDBusMessage::createMethodCall(service, root, "org.freedesktop.DBus.Properties", "Get");
    message.setArguments({iface, QString("version")});
    auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 1000);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
        return 0;
    return qvariant_cast<QDBusVariant>(reply.arguments().first()).variant().toUInt();
}
void PortalRequest::call(QString iface, QString method, QVariantList args, QVariantMap options, Callback cb) {
    callback = std::move(cb);
    QString token = "snapling_" + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = connection.baseService().mid(1).replace('.', '_');
    path = root + "/request/" + sender + "/" + token;
    options["handle_token"] = token;
    auto bus = connection;
    bus.connect(service, path, "org.freedesktop.portal.Request", "Response", this,
                SLOT(response(uint, QVariantMap)));
    auto message = QDBusMessage::createMethodCall(service, root, iface, method);
    args.append(options);
    message.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (done)
            return;
        if (reply.isError()) {
            finish(2, {}, "Portal 调用失败：" + reply.error().name());
            return;
        }
        if (reply.value().path() != path) {
            auto bus = connection;
            bus.disconnect(service, path, "org.freedesktop.portal.Request", "Response", this,
                           SLOT(response(uint, QVariantMap)));
            path = reply.value().path();
            bus.connect(service, path, "org.freedesktop.portal.Request", "Response", this,
                        SLOT(response(uint, QVariantMap)));
        }
    });
    timer.start();
}
void PortalRequest::response(uint code, const QVariantMap &results) {
    finish(code, results);
}
void PortalRequest::cancel() {
    if (done)
        return;
    if (!path.isEmpty()) {
        auto m = QDBusMessage::createMethodCall(service, path, "org.freedesktop.portal.Request", "Close");
        connection.asyncCall(m);
    }
    finish(1, {}, "请求已取消");
}
void PortalRequest::finish(uint code, QVariantMap results, QString error) {
    if (done)
        return;
    done = true;
    timer.stop();
    connection.disconnect(service, path, "org.freedesktop.portal.Request", "Response",
                                             this, SLOT(response(uint, QVariantMap)));
    if (callback)
        callback(code, results, error);
    deleteLater();
}
} // namespace SnapLing

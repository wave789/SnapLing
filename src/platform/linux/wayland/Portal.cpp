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
PortalRequest::PortalRequest(QObject *parent) : QObject(parent) {
    timer.setSingleShot(true);
    timer.setInterval(120000);
    connect(&timer, &QTimer::timeout, this, [this] { cancel(); });
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
    QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
    path = root + "/request/" + sender + "/" + token;
    options["handle_token"] = token;
    auto bus = QDBusConnection::sessionBus();
    bus.connect(service, path, "org.freedesktop.portal.Request", "Response", this,
                SLOT(response(uint, QVariantMap)));
    auto message = QDBusMessage::createMethodCall(service, root, iface, method);
    args.append(options);
    message.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (done)
            return;
        if (reply.isError()) {
            finish(2, {}, reply.error().message());
            return;
        }
        if (reply.value().path() != path) {
            auto bus = QDBusConnection::sessionBus();
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
        QDBusConnection::sessionBus().asyncCall(m);
    }
    finish(1, {}, "请求已取消或超时");
}
void PortalRequest::finish(uint code, QVariantMap results, QString error) {
    if (done)
        return;
    done = true;
    timer.stop();
    QDBusConnection::sessionBus().disconnect(service, path, "org.freedesktop.portal.Request", "Response",
                                             this, SLOT(response(uint, QVariantMap)));
    if (callback)
        callback(code, results, error);
    deleteLater();
}
} // namespace SnapLing

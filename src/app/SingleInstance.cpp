#include "SingleInstance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
namespace SnapLing {
SingleInstance::SingleInstance(QObject *p) : QObject(p) {
    server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server, &QLocalServer::newConnection, this, [this] {
        while (auto *s = server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(s, &QLocalSocket::readyRead, this, [this, s, buffer] {
                *buffer += s->readAll();
                if (buffer->size() > 65536) {
                    s->abort();
                    return;
                }
                int end = buffer->indexOf('\n');
                if (end < 0)
                    return;
                auto doc = QJsonDocument::fromJson(buffer->left(end));
                if (doc.isArray()) {
                    QStringList args;
                    for (auto a : doc.array())
                        args << a.toString();
                    emit command(args);
                    s->write("ok\n");
                    s->flush();
                }
                s->disconnectFromServer();
            });
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            QTimer::singleShot(5000, s, [s] { s->disconnectFromServer(); });
        }
    });
}
SingleInstance::Result SingleInstance::start(const QStringList &args) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    QString name = "snapling_" +
                   QString::fromLatin1(
                       QCryptographicHash::hash(dir.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    lock = std::make_unique<QLockFile>(dir + "/instance.lock");
    if (!lock->tryLock(0)) {
        QLocalSocket socket;
        socket.connectToServer(name);
        if (!socket.waitForConnected(1500))
            return Result::Error;
        QJsonArray a;
        for (auto v : args)
            a.append(v);
        socket.write(QJsonDocument(a).toJson(QJsonDocument::Compact) + "\n");
        if (!socket.waitForBytesWritten(1000))
            return Result::Error;
        if (!socket.waitForReadyRead(1500))
            return Result::Error;
        return socket.readAll().startsWith("ok") ? Result::Delivered : Result::Error;
    }
    QLocalServer::removeServer(name);
    return server.listen(name) ? Result::Primary : Result::Error;
}
} // namespace SnapLing

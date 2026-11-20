#pragma once
#include <QDBusConnection>
#include <QObject>
#include <QPointer>
#include <functional>
class QDBusServiceWatcher;

namespace SnapLing {
// 身份由 Portal 按 D-Bus 唯一连接名缓存；截图使用独立连接，先 Register，再调用 Screenshot。
class PortalConnection final : public QObject {
    Q_OBJECT
  public:
    explicit PortalConnection(QString appId, QObject *parent = nullptr);
    ~PortalConnection() override;
    using Callback = std::function<void(QString error)>;
    void ensureRegistered(QObject *context, Callback callback);
    QDBusConnection bus() const { return connection; }
    bool hasRegisteredIdentity() const { return state == State::Registered; }
  signals:
    void serviceRestarted();
  private:
    enum class State { Unregistered, Registering, Registered, Legacy };
    struct Waiter { QPointer<QObject> context; Callback callback; };
    void complete(State next, QString error = {});
    QString appId, connectionName;
    QDBusConnection connection;
    QDBusServiceWatcher *serviceWatcher = nullptr;
    bool renewBeforeRegistration = false;
    State state = State::Unregistered;
    QList<Waiter> waiters;
    quint64 generation = 0;
};
} // namespace SnapLing

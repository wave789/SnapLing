#pragma once
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <functional>
namespace SnapLing {
// 在调用方法前订阅 Response，避免立即返回时丢失信号。
class PortalRequest final : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(uint, QVariantMap, QString)>;
    explicit PortalRequest(QObject *parent = nullptr);
    void call(QString interface, QString method, QVariantList args, QVariantMap options, Callback cb);
    void cancel();
    static uint version(const QString &interface);
  private slots:
    void response(uint code, const QVariantMap &results);

  private:
    void finish(uint code, QVariantMap results, QString error = {});
    QString path;
    Callback callback;
    QTimer timer;
    bool done = false;
};
} // namespace SnapLing

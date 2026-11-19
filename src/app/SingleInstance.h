#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <memory>
namespace SnapLing {
class SingleInstance final : public QObject {
    Q_OBJECT
  public:
    explicit SingleInstance(QObject *parent = nullptr);
    enum class Result { Primary, Delivered, Error };
    Result start(const QStringList &arguments);
  signals:
    void command(QStringList arguments);

  private:
    QLocalServer server;
    std::unique_ptr<QLockFile> lock;
};
} // namespace SnapLing

#pragma once
#include <QImage>
#include <QMutex>
#include <QObject>
#include <memory>
namespace SnapLing {
class PipeWireStream final : public QObject {
    Q_OBJECT
  public:
    explicit PipeWireStream(QObject *parent = nullptr);
    ~PipeWireStream() override;
    bool start(int fd, uint node, const QString &serial, QString &error);
    void stop();
    QImage latest() const;
  signals:
    void failed(QString error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace SnapLing

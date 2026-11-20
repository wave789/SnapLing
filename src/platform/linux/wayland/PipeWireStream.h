#pragma once
#include <QImage>
#include <QMutex>
#include <QObject>
#include <memory>
namespace SnapLing {
class PipeWireStream : public QObject {
    Q_OBJECT
  public:
    explicit PipeWireStream(QObject *parent = nullptr);
    ~PipeWireStream() override;
    virtual bool start(int fd, uint node, const QString &serial, QString &error);
    virtual void stop();
    virtual QImage latest() const;
  signals:
    void failed(QString error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace SnapLing

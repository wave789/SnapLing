#pragma once
#include "core/Types.h"
#include <QObject>
#include <QThreadPool>
namespace SnapLing {
class ICaptureBackend : public QObject {
    Q_OBJECT
  public:
    explicit ICaptureBackend(QObject *parent = nullptr) : QObject(parent) {
        workerPool.setMaxThreadCount(1);
    }
    ~ICaptureBackend() override {
        workerPool.waitForDone();
    }
    virtual void capture(CaptureMode mode, int screen = -1) = 0;
    virtual void cancel() = 0;
    virtual QString name() const = 0;
    virtual bool hasWindowPicking() const = 0;

  protected:
    QThreadPool workerPool; // 连续截图复用单线程，限制大缓冲区在多个分配器 arena 中滞留。
  signals:
    void captured(SnapLing::CaptureFrame frame);
    void failed(QString message);
};
std::unique_ptr<ICaptureBackend> createCaptureBackend();
QString platformDescription();
} // namespace SnapLing

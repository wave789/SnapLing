#pragma once
#include "core/Types.h"
#include <QObject>
#include <QTimer>
namespace SnapLing {
class IOcrEngine : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void recognize(QImage image, QString modelPath, QString languages, bool cache) = 0;
    virtual void cancel() = 0;
    virtual void release() = 0;
  signals:
    void completed(QVector<SnapLing::OcrLine> lines, qint64 elapsedMs, bool cold);
    void failed(QString message);
    void progress(int percent);
};
// CPU 本地推理，模型只在第一次识别时初始化。
class TesseractOcr final : public IOcrEngine {
    Q_OBJECT
  public:
    explicit TesseractOcr(QObject *parent = nullptr);
    ~TesseractOcr() override;
    void recognize(QImage, QString, QString, bool) override;
    void cancel() override;
    void release() override;

  private:
    struct State;
    std::shared_ptr<State> state;
    Cancellation token;
    bool busy = false;
    QTimer progressTimer;
};
QString findTessdata();
QString modelInfo(QString path);
} // namespace SnapLing

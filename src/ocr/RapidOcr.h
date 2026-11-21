#pragma once
#include "OcrEngine.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

namespace SnapLing {
class RapidOcr final : public IOcrEngine {
  public:
    enum class Profile { Rapid, Paddle };
    explicit RapidOcr(Profile profile = Profile::Rapid, QObject *parent = nullptr);
    static QString launcher(Profile profile);
    ~RapidOcr() override;
    void recognize(QImage image, QString modelPath, QString languages, bool cache) override;
    void cancel() override;
    void release() override {}

  private:
    void startProcess(const QString &helper, const QString &imagePath);
    void abort(QString reason);
    void finish(QVector<OcrLine> lines, QString error);

    Profile profile;
    QString engineName;
    QProcess process;
    QTimer deadline;
    QElapsedTimer elapsed;
    std::shared_ptr<QTemporaryDir> input;
    QSize imageSize;
    QByteArray output, diagnostics;
    QString abortReason;
};
} // namespace SnapLing

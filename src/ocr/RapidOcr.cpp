#include "RapidOcr.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtConcurrent>
#include <cmath>

namespace SnapLing {
namespace {
constexpr qsizetype outputLimit = 8 * 1024 * 1024;
constexpr qsizetype diagnosticLimit = 4096;

bool parseLines(const QByteArray &output, QSize imageSize, QVector<OcrLine> &lines) {
    const auto document = QJsonDocument::fromJson(output);
    const auto results = document.array();
    if (!document.isArray() || results.size() > 10000)
        return false;
    lines.reserve(results.size());
    for (const auto &value : results) {
        const auto result = value.toObject();
        const auto text = result.value("text");
        const auto points = result.value("box").toArray();
        const auto score = result.value("confidence").toDouble(-1);
        if (!text.isString() || points.size() != 4 || !std::isfinite(score) || score < 0 || score > 1)
            return false;

        OcrLine line;
        for (const auto &point : points) {
            const auto xy = point.toArray();
            if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble())
                return false;
            const double x = xy.at(0).toDouble();
            const double y = xy.at(1).toDouble();
            if (!std::isfinite(x) || !std::isfinite(y))
                return false;
            line.polygon.append(QPointF(qBound(0.0, x, double(imageSize.width())),
                                        qBound(0.0, y, double(imageSize.height()))));
        }
        line.text = text.toString().trimmed();
        line.box = line.polygon.boundingRect();
        if (line.text.isEmpty() || line.box.isEmpty())
            continue;
        line.order = static_cast<int>(lines.size());
        line.paragraph = line.order;
        line.id = QString("line_%1").arg(line.order);
        line.confidence = score;
        lines.push_back(std::move(line));
    }
    return true;
}
} // namespace

RapidOcr::RapidOcr(QObject *parent) : IOcrEngine(parent) {
    deadline.setSingleShot(true);
    deadline.setInterval(45000);
    connect(&deadline, &QTimer::timeout, this, [this] { abort("RapidOCR 识别超过 45 秒"); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        const auto chunk = process.readAllStandardOutput();
        if (!abortReason.isEmpty())
            return;
        if (chunk.size() > outputLimit - output.size()) {
            abort("RapidOCR 返回的识别结果过大");
            return;
        }
        output.append(chunk);
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        const auto chunk = process.readAllStandardError();
        diagnostics.append(chunk.first(qMin(chunk.size(), diagnosticLimit - diagnostics.size())));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({}, abortReason.isEmpty() ? "无法启动 RapidOCR，请重新运行 setup-rapidocr.py。" : abortReason);
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!abortReason.isEmpty()) {
            finish({}, abortReason);
            return;
        }
        if (status != QProcess::NormalExit || code != 0) {
            auto detail = QString::fromUtf8(diagnostics).simplified().left(300);
            finish({}, detail.isEmpty() ? "RapidOCR 识别失败，请检查运行环境和模型。" : "RapidOCR：" + detail);
            return;
        }
        QVector<OcrLine> lines;
        if (!parseLines(output, imageSize, lines)) {
            finish({}, "RapidOCR 返回了无法读取的识别结果，请重新运行 setup-rapidocr.py。");
            return;
        }
        finish(std::move(lines), {});
    });
}

RapidOcr::~RapidOcr() {
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
}

void RapidOcr::recognize(QImage image, QString, QString, bool) {
    if (input) {
        emit failed("OCR 正在运行，请先取消或等待完成");
        return;
    }
    if (image.isNull()) {
        emit failed("没有可识别的图像");
        return;
    }
    const auto helper = ocrModelRoot() + "/RapidOCR/run-ocr";
    const QFileInfo file(helper);
    if (!file.isFile() || !file.isExecutable()) {
        emit failed("RapidOCR 尚未安装。请运行 setup-rapidocr.py 安装模型和运行环境，或切换到 Tesseract。");
        return;
    }

    auto workspace = std::make_shared<QTemporaryDir>(QDir::temp().filePath("SnapLing-OCR-XXXXXX"));
    if (!workspace->isValid()) {
        emit failed("无法创建 RapidOCR 临时目录");
        return;
    }
    input = workspace;
    imageSize = image.size();
    elapsed.start();
    deadline.start();
    const auto imagePath = workspace->filePath("input.png");
    auto *writer = new QFutureWatcher<bool>(this);
    connect(writer, &QFutureWatcher<bool>::finished, this, [this, writer, workspace, helper, imagePath] {
        const bool wroteImage = writer->result();
        writer->deleteLater();
        if (input != workspace)
            return;
        if (!wroteImage) {
            finish({}, "无法写入 RapidOCR 临时图片");
            return;
        }
        startProcess(helper, imagePath);
    });
    writer->setFuture(QtConcurrent::run([image = std::move(image), workspace, imagePath] {
        return image.save(imagePath, "PNG");
    }));
}

void RapidOcr::startProcess(const QString &helper, const QString &imagePath) {
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(helper, {imagePath});
}

void RapidOcr::cancel() {
    if (input)
        abort("OCR 已取消");
}

void RapidOcr::abort(QString reason) {
    if (!abortReason.isEmpty())
        return;
    if (process.state() == QProcess::NotRunning) {
        finish({}, std::move(reason));
        return;
    }
    abortReason = std::move(reason);
    process.kill();
}

void RapidOcr::finish(QVector<OcrLine> lines, QString error) {
    deadline.stop();
    input.reset();
    output.clear();
    diagnostics.clear();
    abortReason.clear();
    if (error.isEmpty())
        emit completed(std::move(lines), elapsed.elapsed(), true);
    else
        emit failed(std::move(error));
}
} // namespace SnapLing

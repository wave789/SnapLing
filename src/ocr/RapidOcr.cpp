#include "RapidOcr.h"
#include <QCoreApplication>
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

QString RapidOcr::launcher(Profile profile) {
    const auto configured = qEnvironmentVariable("SNAPLING_PADDLE_RUNTIME");
    if (!configured.isEmpty()) {
        const QFileInfo file(configured);
        if (file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    const auto binary = QCoreApplication::applicationDirPath();
    const QString executable = profile == Profile::Paddle ? "run-paddle-ocr" : "run-ocr";
    const QStringList candidates{
        binary + "/paddleocr-runtime/" + executable,
        QDir::cleanPath(binary + "/../share/snapling/paddleocr-runtime/" + executable),
        "/usr/share/snapling/paddleocr-runtime/" + executable,
        QDir::homePath() + "/.local/snapling/share/snapling/paddleocr-runtime/" + executable};
    for (const auto &path : candidates) {
        const QFileInfo file(path);
        if (file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    return QDir::cleanPath(binary + "/../share/snapling/paddleocr-runtime/" + executable);
}
QString RapidOcr::modelDirectory(Profile profile) {
    if (profile == Profile::Paddle) {
        const auto configured = qEnvironmentVariable("SNAPLING_PADDLE_MODELS");
        const auto binary = QCoreApplication::applicationDirPath();
        const QStringList candidates{
            configured,
            binary + "/../src/assets/models/PaddleOCR",
            QDir::cleanPath(binary + "/../share/snapling/models/paddleocr"),
            "/usr/share/snapling/models/paddleocr",
            QDir::homePath() + "/.local/snapling/share/snapling/models/paddleocr"};
        for (const auto &path : candidates) {
            if (path.isEmpty())
                continue;
            const QDir directory(QDir::cleanPath(path));
            if (QFileInfo(directory.filePath("det/PP-OCRv6_small_det.onnx")).isFile() &&
                QFileInfo(directory.filePath("det/inference.yml")).isFile() &&
                QFileInfo(directory.filePath("rec/PP-OCRv6_small_rec.onnx")).isFile() &&
                QFileInfo(directory.filePath("rec/inference.yml")).isFile())
                return directory.absolutePath();
        }
        return QDir::cleanPath(binary + "/../share/snapling/models/paddleocr");
    }
    return QFileInfo(launcher(profile)).dir().absoluteFilePath("models");
}

RapidOcr::RapidOcr(Profile selected, QObject *parent)
    : IOcrEngine(parent), profile(selected), engineName(selected == Profile::Paddle ? "PaddleOCR" : "RapidOCR") {
    deadline.setSingleShot(true);
    deadline.setInterval(45000);
    connect(&deadline, &QTimer::timeout, this, [this] { abort(engineName + " 识别超过 45 秒"); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        const auto chunk = process.readAllStandardOutput();
        if (!abortReason.isEmpty())
            return;
        if (chunk.size() > outputLimit - output.size()) {
            abort(engineName + " 返回的识别结果过大");
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
            finish({}, abortReason.isEmpty() ? "无法启动内置 " + engineName + "，请重新安装 SnapLing。" : abortReason);
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!abortReason.isEmpty()) {
            finish({}, abortReason);
            return;
        }
        if (status != QProcess::NormalExit || code != 0) {
            auto detail = QString::fromUtf8(diagnostics).simplified().left(300);
            finish({}, detail.isEmpty() ? engineName + " 识别失败，请检查运行环境和模型。" : engineName + "：" + detail);
            return;
        }
        QVector<OcrLine> lines;
        if (!parseLines(output, imageSize, lines)) {
            finish({}, "内置 " + engineName + " 返回了无法读取的识别结果，请重新安装 SnapLing。");
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
    const auto helper = launcher(profile);
    const QFileInfo file(helper);
    if (!file.isFile() || !file.isExecutable()) {
        emit failed("未找到内置 " + engineName + " 运行环境。请使用包含 OCR 运行时的 SnapLing 安装包重新安装。");
        return;
    }

    auto workspace = std::make_shared<QTemporaryDir>(QDir::temp().filePath("SnapLing-OCR-XXXXXX"));
    if (!workspace->isValid()) {
        emit failed("无法创建 " + engineName + " 临时目录");
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
            finish({}, "无法写入 " + engineName + " 临时图片");
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
    QStringList arguments{imagePath};
    if (profile == Profile::Paddle)
        arguments.prepend(modelDirectory(profile));
    process.start(helper, arguments);
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

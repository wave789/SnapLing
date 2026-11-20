#include "OcrEngine.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMutex>
#include <QPainter>
#include <QStandardPaths>
#include <QtConcurrent>
#ifdef SNAPLING_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>
#endif
namespace SnapLing {
#ifdef SNAPLING_HAVE_TESSERACT
struct OcrInput {
    QImage pixels;
    qreal scale = 1;
    int border = 12;
    QRectF sourceRect(int left, int top, int right, int bottom, QSize sourceSize) const {
        return QRectF((left - border) / scale, (top - border) / scale,
                      (right - left) / scale, (bottom - top) / scale)
            .intersected(QRectF(QPointF(), sourceSize));
    }
};
static OcrInput prepareScreenshot(const QImage &image) {
    QImage opaque(image.size(), QImage::Format_RGB32);
    opaque.fill(Qt::white);
    {
        QPainter painter(&opaque);
        painter.drawImage(opaque.rect(), image);
    }
    auto gray = opaque.convertToFormat(QImage::Format_Grayscale8);
    qint64 dark = 0, samples = 0;
    for (int y = 0; y < gray.height(); y += 4)
        for (int x = 0; x < gray.width(); x += 4) {
            dark += gray.constScanLine(y)[x] < 128;
            ++samples;
        }
    if (dark > samples * .6)
        gray.invertPixels();
    OcrInput input;
    // 小字号界面放大后识别；限制工作图像大小，坐标统一映射回原图像素。
    input.scale = qint64(image.width()) * image.height() <= 4 * 1024 * 1024 ? 2 : 1;
    if (input.scale > 1)
        gray = gray.scaled(gray.size() * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    input.pixels = QImage(gray.size() + QSize(input.border * 2, input.border * 2), QImage::Format_RGB32);
    input.pixels.fill(Qt::white);
    {
        QPainter painter(&input.pixels);
        painter.drawImage(input.border, input.border, gray);
    }
    input.pixels = input.pixels.convertToFormat(QImage::Format_Grayscale8);
    return input;
}
#endif
QString ocrModelRoot() {
    const auto configured = qEnvironmentVariable("SNAPLING_OCR_HOME");
    return QDir::isAbsolutePath(configured) ? QDir::cleanPath(configured)
                                          : QDir::homePath() + "/.snapling/ocr";
}
QString userTessdataDirectory() {
    return ocrModelRoot() + "/tesseract";
}
QString availableTessdata(QString path, const QString &languages) {
    path = path.trimmed();
    if (path.isEmpty())
        return {};
    if (path == "~")
        path = QDir::homePath();
    else if (path.startsWith("~/"))
        path.replace(0, 1, QDir::homePath());
    const QDir directory(path);
    for (const auto &language : languages.split('+')) {
        if (language.isEmpty())
            return {};
        const auto filePath = directory.filePath(language + ".traineddata");
        const QFileInfo info(filePath);
        QFile file(filePath);
        if (!info.isFile() || info.size() <= 0 || !file.open(QIODevice::ReadOnly))
            return {};
    }
    return QDir::cleanPath(directory.absolutePath());
}
static QStringList bundledTessdataDirs() {
    const auto binary = QCoreApplication::applicationDirPath();
    return {QDir::cleanPath(binary + "/../share/snapling/tessdata"), binary + "/tessdata",
            QDir::cleanPath(binary + "/../models/tessdata")};
}
QString findTessdata(const QString &languages) {
    const QStringList paths = QStringList{userTessdataDirectory(), qEnvironmentVariable("TESSDATA_PREFIX")} +
                              bundledTessdataDirs() +
                              QStringList{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tessdata",
                                          QCoreApplication::applicationDirPath() + "/../.deps/usr/share/tesseract-ocr/5/tessdata",
                                          "/usr/share/tesseract-ocr/5/tessdata", "/usr/share/tessdata"};
    for (const auto &path : paths) {
        const auto available = availableTessdata(path, languages);
        if (!available.isEmpty())
            return available;
    }
    return {};
}
#ifdef SNAPLING_HAVE_TESSERACT
static QString prepareTessdata(const QString &configuredPath, const QString &languages) {
    const auto selected = availableTessdata(configuredPath, languages);
    if (!selected.isEmpty())
        return selected;
    const auto automatic = findTessdata(languages);
    if (!bundledTessdataDirs().contains(automatic) ||
        automatic == availableTessdata(qEnvironmentVariable("TESSDATA_PREFIX"), languages))
        return automatic;
    const QDir destination(userTessdataDirectory());
    if (!QDir().mkpath(destination.absolutePath()))
        return automatic;

    // Copy bundled defaults once and retain any models the user supplied.
    for (const QString &language : {QStringLiteral("eng"), QStringLiteral("chi_sim")}) {
        const auto file = language + ".traineddata";
        if (!destination.exists(file))
            QFile::copy(QDir(automatic).filePath(file), destination.filePath(file));
    }
    const auto installed = availableTessdata(destination.absolutePath(), languages);
    return installed.isEmpty() ? automatic : installed;
}
#endif
QString modelInfo(QString path) {
    if (path.isEmpty())
        return "未找到所选语言的 Tesseract 模型。";
    QStringList info;
    for (QString language : {"eng", "chi_sim"}) {
        QFile f(path + "/" + language + ".traineddata");
        if (!f.open(QIODevice::ReadOnly)) {
            info << language + ": 缺失";
            continue;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&f);
        info << QString("%1 · %2 bytes · SHA256 %3")
                    .arg(language)
                    .arg(f.size())
                    .arg(QString::fromLatin1(hash.result().toHex()));
    }
    return info.join('\n');
}
struct TesseractOcr::State {
    QMutex mutex;
#ifdef SNAPLING_HAVE_TESSERACT
    std::unique_ptr<tesseract::TessBaseAPI> api;
#endif
    QString path, languages;
};
TesseractOcr::TesseractOcr(QObject *p) : IOcrEngine(p), state(std::make_shared<State>()) {
    progressTimer.setInterval(200);
    connect(&progressTimer, &QTimer::timeout, this, [this] {
        if (token)
            emit progress(token->progress.load());
    });
}
TesseractOcr::~TesseractOcr() {
    cancel();
}
void TesseractOcr::cancel() {
    if (token)
        token->cancel();
}
void TesseractOcr::release() {
    if (busy)
        return;
    auto s = state;
    (void)QtConcurrent::run([s] {
        QMutexLocker lock(&s->mutex);
#ifdef SNAPLING_HAVE_TESSERACT
        s->api.reset();
#endif
    });
}
void TesseractOcr::recognize(QImage image, QString path, QString languages, bool cache) {
#ifndef SNAPLING_HAVE_TESSERACT
    Q_UNUSED(image)
    Q_UNUSED(path)
    Q_UNUSED(languages)
    Q_UNUSED(cache)
    emit failed("此构建未包含 OCR 引擎");
#else
    if (busy) {
        emit failed("OCR 正在运行，请先取消或等待完成");
        return;
    }
    if (image.isNull()) {
        emit failed("没有可识别的图像");
        return;
    }
    token = std::make_shared<CancelToken>();
    auto t = token;
    auto s = state;
    busy = true;
    progressTimer.start();
    emit progress(0);
    struct Result {
        QVector<OcrLine> lines;
        QString error;
        qint64 ms = 0;
        bool cold = true;
    };
    auto *w = new QFutureWatcher<Result>(this);
    connect(w, &QFutureWatcher<Result>::finished, this, [this, w, t] {
        auto r = w->result();
        w->deleteLater();
        busy = false;
        progressTimer.stop();
        if (t->isCancelled()) {
            emit failed("OCR 已取消");
            return;
        }
        if (!r.error.isEmpty())
            emit failed(r.error);
        else {
            emit progress(100);
            emit completed(r.lines, r.ms, r.cold);
        }
    });
    w->setFuture(QtConcurrent::run([s, t, image = std::move(image), configuredPath = path.trimmed(), languages, cache] {
        Result r;
        QElapsedTimer timer;
        timer.start();
        const auto path = prepareTessdata(configuredPath, languages);
        if (path.isEmpty()) {
            const auto location = configuredPath.isEmpty() ? QString() : "已设置目录：" + configuredPath + "\n";
            r.error = location + "缺少 OCR 模型（" + languages + "）。请还原项目自带模型，"
                      "或在设置 → OCR 中选择包含所需 .traineddata 文件的目录。用户模型目录：" +
                      userTessdataDirectory();
            return r;
        }
        QMutexLocker lock(&s->mutex);
        r.cold = !s->api || s->path != path || s->languages != languages;
        if (r.cold) {
            s->api = std::make_unique<tesseract::TessBaseAPI>();
            if (s->api->Init(path.toUtf8().constData(), languages.toUtf8().constData(),
                             tesseract::OEM_LSTM_ONLY) != 0) {
                s->api.reset();
                r.error = "本地 OCR 模型初始化失败，检查模型格式和路径";
                return r;
            }
            s->path = path;
            s->languages = languages;
            s->api->SetVariable("preserve_interword_spaces", "1");
        }
        if (t->isCancelled())
            return r;
        const auto input = prepareScreenshot(image);
        // 截图包含浮层、菜单和多列文字，不能按单张连续文档合并各窗口的文本行。
        s->api->SetPageSegMode(tesseract::PSM_SPARSE_TEXT);
        s->api->SetImage(input.pixels.constBits(), input.pixels.width(), input.pixels.height(), 1,
                         int(input.pixels.bytesPerLine()));
        s->api->SetSourceResolution(qRound(144 * input.scale));
        tesseract::ETEXT_DESC monitor;
        monitor.cancel_this = t.get();
        monitor.cancel = [](void *p, int) { return static_cast<CancelToken *>(p)->isCancelled(); };
        monitor.progress_callback2 = [](tesseract::ETEXT_DESC *m, int, int, int, int) {
            static_cast<CancelToken *>(m->cancel_this)->progress.store(m->progress);
            return true;
        };
        monitor.set_deadline_msecs(45000);
        if (s->api->Recognize(&monitor) != 0) {
            r.error = t->isCancelled() ? "OCR 已取消" : "OCR 识别失败或超过 45 秒";
            s->api->Clear();
            return r;
        }
        std::unique_ptr<tesseract::ResultIterator> it(s->api->GetIterator());
        int order = 0, paragraph = -1;
        OcrLine run;
        QRectF previousWord;
        double confidenceSum = 0;
        int characters = 0;
        auto flush = [&] {
            if (!run.text.isEmpty()) {
                run.id = QString("line_%1").arg(order);
                run.order = order++;
                run.polygon = QPolygonF(run.box);
                run.confidence = confidenceSum / qMax(1, characters);
                r.lines << run;
            }
            run = {};
            previousWord = {};
            confidenceSum = 0;
            characters = 0;
        };
        if (it)
            do {
                if (t->isCancelled())
                    break;
                if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE))
                    flush();
                if (it->IsAtBeginningOf(tesseract::RIL_PARA))
                    ++paragraph;
                std::unique_ptr<char[]> text(it->GetUTF8Text(tesseract::RIL_WORD));
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                if (!text || !it->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2))
                    continue;
                const auto value = QString::fromUtf8(text.get()).trimmed();
                const auto box = input.sourceRect(x1, y1, x2, y2, image.size());
                if (value.isEmpty() || box.isEmpty())
                    continue;
                if (!previousWord.isEmpty()) {
                    const auto gap = qMax(box.left() - previousWord.right(), previousWord.left() - box.right());
                    const auto height = qMax(box.height(), previousWord.height());
                    if (gap > qMax(16., height * 2.5) ||
                        qAbs(box.center().y() - previousWord.center().y()) > height)
                        flush();
                }
                if (!run.text.isEmpty() && it->BlanksBeforeWord() > 0 &&
                    !(run.text.back().script() == QChar::Script_Han && value.front().script() == QChar::Script_Han))
                    run.text += QString(qMin(8, it->BlanksBeforeWord()), ' ');
                run.text += value;
                run.box = run.box.united(box);
                run.paragraph = qMax(0, paragraph);
                confidenceSum += it->Confidence(tesseract::RIL_WORD) / 100. * value.size();
                characters += value.size();
                previousWord = box;
            } while (it->Next(tesseract::RIL_WORD));
        flush();
        s->api->Clear();
        if (!cache)
            s->api.reset();
        r.ms = timer.elapsed();
        return r;
    }));
#endif
}
} // namespace SnapLing

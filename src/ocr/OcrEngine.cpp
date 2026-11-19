#include "OcrEngine.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QMutex>
#include <QStandardPaths>
#include <QtConcurrent>
#ifdef SNAPLING_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>
#endif
namespace SnapLing {
QString findTessdata() {
    QStringList paths{qEnvironmentVariable("TESSDATA_PREFIX"),
                      QCoreApplication::applicationDirPath() + "/../share/snapling/tessdata",
                      QCoreApplication::applicationDirPath() + "/tessdata",
                      QCoreApplication::applicationDirPath() + "/../models/tessdata",
                      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tessdata",
                      "/usr/share/tesseract-ocr/5/tessdata",
                      "/usr/share/tessdata"};
    for (auto path : paths)
        if (!path.isEmpty() && QFile::exists(path + "/eng.traineddata"))
            return QDir(path).absolutePath();
    return {};
}
QString modelInfo(QString path) {
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
    if (busy) {
        emit failed("OCR 正在运行，请先取消或等待完成");
        return;
    }
    if (image.isNull()) {
        emit failed("没有可识别的图像");
        return;
    }
    if (path.isEmpty())
        path = findTessdata();
    for (auto lang : languages.split('+'))
        if (!QFile::exists(path + "/" + lang + ".traineddata")) {
            emit failed("OCR 模型缺失：" + lang + "。请在设置中导入可信的 traineddata 模型。");
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
    w->setFuture(QtConcurrent::run([s, t, image = std::move(image), path, languages, cache] {
        Result r;
        QElapsedTimer timer;
        timer.start();
        QMutexLocker lock(&s->mutex);
#ifdef SNAPLING_HAVE_TESSERACT
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
        auto rgb = image.convertToFormat(QImage::Format_RGB888);
        s->api->SetPageSegMode(tesseract::PSM_AUTO);
        s->api->SetImage(rgb.constBits(), rgb.width(), rgb.height(), 3, int(rgb.bytesPerLine()));
        s->api->SetSourceResolution(144);
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
        if (it)
            do {
                if (t->isCancelled())
                    break;
                if (it->IsAtBeginningOf(tesseract::RIL_PARA))
                    ++paragraph;
                std::unique_ptr<char[]> text(it->GetUTF8Text(tesseract::RIL_TEXTLINE));
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                if (!text || !it->BoundingBox(tesseract::RIL_TEXTLINE, &x1, &y1, &x2, &y2))
                    continue;
                QString value = QString::fromUtf8(text.get());
                while (value.endsWith('\n') || value.endsWith('\r'))
                    value.chop(1);
                if (value.trimmed().isEmpty())
                    continue;
                OcrLine line;
                line.id = QString("line_%1").arg(order);
                line.text = value;
                line.box = QRectF(x1, y1, x2 - x1, y2 - y1);
                line.polygon = QPolygonF(line.box);
                line.confidence = it->Confidence(tesseract::RIL_TEXTLINE) / 100.;
                line.order = order++;
                line.paragraph = qMax(0, paragraph);
                r.lines << line;
            } while (it->Next(tesseract::RIL_TEXTLINE));
        s->api->Clear();
        if (!cache)
            s->api.reset();
#else
        Q_UNUSED(image)
        Q_UNUSED(cache)
        r.error = "此构建未包含 OCR 引擎";
#endif
        r.ms = timer.elapsed();
        return r;
    }));
}
} // namespace SnapLing

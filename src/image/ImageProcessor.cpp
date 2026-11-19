#include "ImageProcessor.h"
#include <QFontMetricsF>
#ifdef SNAPLING_HAVE_ZXING
#include <ZXing/ReadBarcode.h>
#include <ZXing/ReaderOptions.h>
#endif
namespace SnapLing {
QColor BasicImageProcessor::background(const QImage &image, QRect box, bool &complex) const {
    QVector<QColor> samples;
    auto r = box.adjusted(-3, -3, 3, 3).intersected(image.rect());
    for (int x = r.left(); x <= r.right(); x += qMax(1, r.width() / 50)) {
        samples << image.pixelColor(x, r.top()) << image.pixelColor(x, r.bottom());
    }
    for (int y = r.top(); y <= r.bottom(); y += qMax(1, r.height() / 50)) {
        samples << image.pixelColor(r.left(), y) << image.pixelColor(r.right(), y);
    }
    if (samples.isEmpty()) {
        complex = true;
        return Qt::white;
    }
    auto median = [&](auto getter) {
        QVector<int> c;
        for (auto color : samples)
            c << getter(color);
        std::sort(c.begin(), c.end());
        return c[c.size() / 2];
    };
    QColor color(median([](QColor c) { return c.red(); }), median([](QColor c) { return c.green(); }),
                 median([](QColor c) { return c.blue(); }));
    double variance = 0;
    for (auto c : samples)
        variance +=
            qAbs(c.red() - color.red()) + qAbs(c.green() - color.green()) + qAbs(c.blue() - color.blue());
    complex = variance / samples.size() > 35;
    return color;
}
void BasicImageProcessor::cover(QPainter &p, const QImage &image, QRect rect, bool panel) const {
    bool complex = false;
    auto color = background(image, rect, complex);
    if (complex || panel)
        color = QColor(248, 248, 248, 250);
    p.fillRect(rect, color);
}
TextFit fitTranslation(QString text, QRectF box, QSize size, int requested) {
    TextFit f;
    f.text = std::move(text);
    f.box = box.intersected(QRectF(QPointF(0, 0), size));
    f.font = QFont("Noto Sans CJK SC");
    int maximum = requested > 0 ? requested : qBound(10, qRound(box.height() * .8), 64);
    auto fits = [&](int px) {
        f.font.setPixelSize(px);
        QFontMetricsF fm(f.font);
        auto b = fm.boundingRect(QRectF(0, 0, qMax(1., f.box.width() - 4), 100000),
                                 Qt::TextWordWrap | Qt::TextWrapAnywhere, f.text);
        return b.height() <= f.box.height() - 2 && b.width() <= f.box.width() + 1;
    };
    for (int px = maximum; px >= 8; --px)
        if (fits(px))
            return f;
    // 文字不能容纳时，扩展到图像边界；仍不够则明确标记，不静默截断。
    f.box.setWidth(qMin(double(size.width()) - f.box.x(), qMax(f.box.width(), box.width() * 1.7)));
    QFontMetricsF fm(f.font);
    auto required = fm.boundingRect(QRectF(0, 0, qMax(1., f.box.width() - 4), 100000),
                                    Qt::TextWordWrap | Qt::TextWrapAnywhere, f.text);
    f.box.setHeight(qMin(double(size.height()) - f.box.y(), qMax(f.box.height(), required.height() + 4)));
    f.expanded = true;
    f.overflow = !fits(8);
    return f;
}
void drawTranslations(QPainter &p, const QImage &source, const QVector<TranslationUnit> &units, bool panel) {
    BasicImageProcessor processor;
    for (const auto &u : units) {
        if (u.status != "done" || u.target.isEmpty())
            continue;
        auto fit = fitTranslation(u.target, u.box, source.size(), u.fontSize);
        processor.cover(p, source, fit.box.toAlignedRect(), panel);
        bool complex = false;
        auto bg = processor.background(source, u.box.toAlignedRect(), complex);
        int gray = qGray(bg.rgb());
        p.setPen((complex || panel || gray > 128) ? Qt::black : Qt::white);
        p.setFont(fit.font);
        p.drawText(fit.box.adjusted(2, 1, -2, -1),
                   Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignLeft | Qt::AlignTop, u.target);
        if (fit.overflow) {
            p.setPen(QPen(Qt::red, 2, Qt::DashLine));
            p.drawRect(fit.box);
        }
    }
}
QImage redactPixels(const QImage &image, QRect r, int block, bool blur) {
    r = r.intersected(image.rect());
    if (r.isEmpty())
        return {};
    auto piece = image.copy(r);
    int step = qMax(2, block);
    return piece
        .scaled(qMax(1, piece.width() / step), qMax(1, piece.height() / step), Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation)
        .scaled(piece.size(), Qt::IgnoreAspectRatio,
                blur ? Qt::SmoothTransformation : Qt::FastTransformation);
}
QStringList decodeQr(const QImage &image, QString &error) {
    error.clear();
#ifdef SNAPLING_HAVE_ZXING
    auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(), ZXing::ImageFormat::Lum,
                          int(gray.bytesPerLine()));
    auto results = ZXing::ReadBarcodes(
        view, ZXing::ReaderOptions().setFormats(ZXing::BarcodeFormat::QRCode).setTryRotate(true));
    QStringList out;
    for (const auto &r : results)
        if (r.isValid())
            out << QString::fromStdString(r.text());
    return out;
#else
    Q_UNUSED(image)
    error = "此构建未包含 ZXing 二维码库";
    return {};
#endif
}
} // namespace SnapLing

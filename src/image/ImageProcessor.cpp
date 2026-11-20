#include "ImageProcessor.h"
#include <QFontMetricsF>
#include <QTextLayout>
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
QColor BasicImageProcessor::cover(QPainter &p, const QImage &image, QRect rect, bool panel) const {
    bool complex = false;
    auto color = background(image, rect, complex);
    if (complex || panel)
        color = qGray(color.rgb()) < 128 ? QColor(37, 39, 43) : QColor(248, 248, 248);
    color.setAlpha(255);
    p.fillRect(rect, color);
    return color;
}
bool rendersTranslation(const TranslationUnit &unit) {
    return unit.status == "done" && !unit.target.trimmed().isEmpty() &&
           unit.target.trimmed() != unit.source.trimmed();
}
TextFit fitTranslation(QString text, QRectF box, QSize size, int requested) {
    TextFit f;
    f.text = std::move(text);
    f.box = box.intersected(QRectF(QPointF(0, 0), size));
    f.font = QFont("Noto Sans CJK SC");
    const int maximum = requested > 0 ? qBound(8, requested, 96) : qBound(8, qRound(box.height()), 64);
    const auto content = f.box.adjusted(1, 0, -1, 0);
    auto layoutText = f.text;
    layoutText.replace("\r\n", "\n");
    layoutText.replace('\n', QChar::LineSeparator);
    auto fits = [&](int px) {
        f.font.setPixelSize(px);
        f.lines.clear();
        if (content.isEmpty())
            return false;
        QFontMetricsF fm(f.font);
        QTextLayout layout(layoutText, f.font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        QRectF ink;
        qreal y = 0;
        bool complete = true;
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(content.width());
            auto value = layoutText.mid(line.textStart(), line.textLength());
            value.remove(QChar::LineSeparator);
            const QPointF baseline(0, y + line.ascent());
            ink = ink.united(fm.tightBoundingRect(value).translated(baseline));
            f.lines << TranslationTextLine{value, baseline};
            y += line.height();
            if (ink.height() > content.height() || ink.width() > content.width()) {
                complete = false;
                break;
            }
        }
        layout.endLayout();
        if (!complete)
            return false;
        // 测量与绘制共享同一组换行和基线，按字形边界适配 OCR 的实际文字框。
        for (auto &line : f.lines)
            line.baseline += content.topLeft() - ink.topLeft();
        return true;
    };
    const int minimum = requested > 0 ? qMax(8, qRound(maximum * .85)) : 8;
    for (int px = maximum; px >= minimum; --px)
        if (fits(px))
            return f;
    // 自动排版始终限制在当前文字框；放不下时保留原文，交由用户明确调整区域。
    f.lines.clear();
    f.overflow = true;
    return f;
}
void drawTranslations(QPainter &p, const QImage &source, const QVector<TranslationUnit> &units, bool panel) {
    BasicImageProcessor processor;
    for (const auto &u : units) {
        if (!rendersTranslation(u))
            continue;
        auto fit = fitTranslation(u.target, u.box, source.size(), u.fontSize);
        p.save();
        p.setClipRect(fit.box, Qt::IntersectClip);
        if (fit.overflow) {
            p.setPen(QPen(Qt::red, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(fit.box.adjusted(1, 1, -1, -1));
            p.restore();
            continue;
        }
        const auto bg = processor.cover(p, source, fit.box.toAlignedRect(), panel);
        p.setPen(qGray(bg.rgb()) > 128 ? Qt::black : Qt::white);
        p.setFont(fit.font);
        for (const auto &line : fit.lines)
            p.drawText(line.baseline, line.text);
        p.restore();
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

#include "TextLayout.h"
#include <QFontMetricsF>
#include <QTextBoundaryFinder>
#include <QTransform>
#include <limits>
namespace SnapLing {
void TextLayout::setLines(QVector<OcrLine> lines) {
    data = std::move(lines);
    std::stable_sort(data.begin(), data.end(), [](auto &a, auto &b) { return a.order < b.order; });
}
QVector<QString> TextLayout::characters(int i) const {
    QVector<QString> out;
    if (i < 0 || i >= data.size())
        return out;
    auto text = data[i].text;
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, text);
    int start = 0, end;
    while ((end = f.toNextBoundary()) >= 0) {
        out << text.mid(start, end - start);
        start = end;
    }
    return out;
}
QVector<QRectF> TextLayout::characterBoxes(int i) const {
    auto chars = characters(i);
    QVector<QRectF> out;
    if (chars.isEmpty())
        return out;
    auto b = data[i].box;
    QFont font;
    font.setPixelSize(qMax(1, qRound(b.height())));
    QFontMetricsF metrics(font);
    QVector<qreal> widths;
    qreal total = 0;
    for (auto c : chars) {
        qreal w = qMax(1., metrics.horizontalAdvance(c));
        widths << w;
        total += w;
    }
    qreal x = b.left();
    for (auto w : widths) {
        qreal width = b.width() * w / total;
        out << QRectF(x, b.y(), width, b.height());
        x += width;
    }
    return out;
}
TextPosition TextLayout::hit(QPointF point, bool nearest) const {
    TextPosition best;
    qreal distance = std::numeric_limits<qreal>::max();
    for (int i = 0; i < data.size(); ++i) {
        auto boxes = characterBoxes(i);
        for (int j = 0; j < boxes.size(); ++j) {
            auto b = boxes[j];
            if (b.contains(point))
                return {i, j};
            if (nearest) {
                qreal d = QLineF(b.center(), point).length();
                if (d < distance) {
                    distance = d;
                    best = {i, j};
                }
            }
        }
    }
    return best;
}
QString TextLayout::selected(TextPosition a, TextPosition b) const {
    if (!a.valid() || !b.valid())
        return {};
    if (a > b)
        std::swap(a, b);
    QStringList out;
    for (int i = a.line; i <= b.line && i < data.size(); ++i) {
        auto chars = characters(i);
        int start = i == a.line ? a.index : 0, end = i == b.line ? b.index : chars.size() - 1;
        QString line;
        for (int j = qMax(0, start); j <= end && j < chars.size(); ++j)
            line += chars[j];
        out << line;
    }
    return out.join('\n');
}
QString TextLayout::all() const {
    QStringList lines;
    for (auto &l : data)
        lines << l.text;
    return lines.join('\n');
}
QVector<QRectF> TextLayout::selectionRects(TextPosition a, TextPosition b) const {
    QVector<QRectF> out;
    if (!a.valid() || !b.valid())
        return out;
    if (a > b)
        std::swap(a, b);
    for (int i = a.line; i <= b.line && i < data.size(); ++i) {
        auto boxes = characterBoxes(i);
        QRectF r;
        for (int j = i == a.line ? a.index : 0;
             j < boxes.size() && j <= (i == b.line ? b.index : boxes.size() - 1); ++j)
            if (j >= 0)
                r = r.united(boxes[j]);
        if (!r.isEmpty())
            out << r;
    }
    return out;
}
QVector<OcrLine> mapOcr(QVector<OcrLine> lines, QTransform t) {
    for (auto &l : lines) {
        l.box = t.mapRect(l.box);
        l.polygon = t.map(l.polygon);
    }
    return lines;
}
} // namespace SnapLing

#pragma once
#include <QTransform>

#include "core/Types.h"
namespace SnapLing {
struct TextPosition {
    int line = -1, index = -1;
    bool valid() const {
        return line >= 0 && index >= 0;
    }
    auto operator<=>(const TextPosition &) const = default;
};
class TextLayout {
  public:
    void setLines(QVector<OcrLine> lines);
    const QVector<OcrLine> &lines() const {
        return data;
    }
    QVector<QString> characters(int line) const;
    QVector<QRectF> characterBoxes(int line) const;
    TextPosition hit(QPointF point, bool nearest = false) const;
    QString selected(TextPosition a, TextPosition b) const;
    QString all() const;
    QVector<QRectF> selectionRects(TextPosition a, TextPosition b) const;

  private:
    QVector<OcrLine> data;
};
QVector<OcrLine> mapOcr(QVector<OcrLine> lines, QTransform transform);
} // namespace SnapLing

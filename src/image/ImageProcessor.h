#pragma once
#include "core/Types.h"
#include <QPainter>
namespace SnapLing {
struct TextFit {
    QRectF box;
    QFont font;
    QString text;
    bool expanded = false, overflow = false;
};
class IImageProcessor {
  public:
    virtual ~IImageProcessor() = default;
    virtual QColor background(const QImage &image, QRect rect, bool &complex) const = 0;
    virtual void cover(QPainter &p, const QImage &image, QRect rect, bool panel) const = 0;
};
class BasicImageProcessor final : public IImageProcessor {
  public:
    QColor background(const QImage &, QRect, bool &) const override;
    void cover(QPainter &, const QImage &, QRect, bool panel) const override;
};
TextFit fitTranslation(QString text, QRectF box, QSize imageSize, int requestedSize = 0);
void drawTranslations(QPainter &p, const QImage &source, const QVector<TranslationUnit> &units, bool panel);
QImage redactPixels(const QImage &image, QRect rect, int block, bool blur);
QStringList decodeQr(const QImage &image, QString &error);
} // namespace SnapLing

#include "Types.h"
#include <QJsonArray>
#include <QtMath>
namespace SnapLing {
bool CaptureFrame::hasSelectionViewport() const {
    return selectionComplete && !image.isNull() && selectionViewport.isValid() &&
           qIsFinite(selectionViewport.left()) && qIsFinite(selectionViewport.top()) &&
           qIsFinite(selectionViewport.right()) && qIsFinite(selectionViewport.bottom()) &&
           QRectF(QPointF(), desktop.size()).contains(selectionViewport);
}
QPointF CaptureFrame::toPixel(QPointF p) const {
    const auto bounds = fullMonitorImage || !hasSelectionViewport()
                            ? desktop
                            : selectionViewport.translated(desktop.topLeft());
    if (bounds.isEmpty())
        return {};
    return {(p.x() - bounds.x()) * image.width() / bounds.width(),
            (p.y() - bounds.y()) * image.height() / bounds.height()};
}
QRect CaptureFrame::pixelRect(QRectF r) const {
    return QRectF(toPixel(r.topLeft()), toPixel(r.bottomRight())).toAlignedRect().intersected(image.rect());
}
QJsonObject rectJson(QRectF r) {
    return {{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}
QRectF jsonRect(const QJsonObject &o) {
    return {o["x"].toDouble(), o["y"].toDouble(), o["w"].toDouble(), o["h"].toDouble()};
}
QJsonObject ocrJson(const OcrLine &l) {
    QJsonArray p;
    for (auto v : l.polygon)
        p.append(QJsonArray{v.x(), v.y()});
    return {{"id", l.id},
            {"text", l.text},
            {"box", rectJson(l.box)},
            {"polygon", p},
            {"confidence", l.confidence},
            {"order", l.order},
            {"paragraph", l.paragraph},
            {"direction", l.direction},
            {"estimated", l.estimatedCharacters}};
}
OcrLine jsonOcr(const QJsonObject &o) {
    OcrLine l;
    l.id = o["id"].toString();
    l.text = o["text"].toString();
    l.box = jsonRect(o["box"].toObject());
    l.confidence = o["confidence"].toDouble();
    l.order = o["order"].toInt();
    l.paragraph = o["paragraph"].toInt();
    l.direction = o["direction"].toInt();
    l.estimatedCharacters = o["estimated"].toBool(true);
    for (auto p : o["polygon"].toArray()) {
        auto a = p.toArray();
        if (a.size() == 2)
            l.polygon << QPointF(a[0].toDouble(), a[1].toDouble());
    }
    return l;
}
QJsonObject unitJson(const TranslationUnit &u) {
    return {
        {"id", u.id},         {"source", u.source},     {"target", u.target},     {"language", u.language},
        {"status", u.status}, {"box", rectJson(u.box)}, {"fontSize", u.fontSize}, {"overflow", u.overflow}};
}
TranslationUnit jsonUnit(const QJsonObject &o) {
    TranslationUnit u;
    u.id = o["id"].toString();
    u.source = o["source"].toString();
    u.target = o["target"].toString();
    u.language = o["language"].toString();
    u.status = o["status"].toString();
    u.box = jsonRect(o["box"].toObject());
    u.fontSize = o["fontSize"].toInt();
    u.overflow = o["overflow"].toBool();
    return u;
}
} // namespace SnapLing

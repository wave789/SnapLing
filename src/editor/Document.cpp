#include "Document.h"
#include "image/ImageProcessor.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <cmath>
namespace SnapLing {
QRectF Annotation::bounds() const {
    auto r = points.boundingRect();
    if (tool == Tool::Text)
        return QRectF(r.topLeft(),
                      QSizeF(qMax(40, text.size() * font.pixelSize()), qMax(24, font.pixelSize() * 2)));
    if (tool == Tool::Number)
        return QRectF(r.topLeft() - QPointF(15, 15), QSizeF(30, 30));
    return r.normalized().adjusted(-width, -width, width, width);
}
Document::Document(QImage i, QObject *p) : QObject(p) {
    reset(std::move(i));
}
void Document::reset(QImage image) {
    source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    source.setDevicePixelRatio(1);
    current = {};
    current.crop = source.rect();
    past.clear();
    future.clear();
    emit changed();
}
void Document::change(std::function<void(DocumentState &)> edit) {
    past.append(current);
    if (past.size() > 100)
        past.removeFirst();
    edit(current);
    future.clear();
    emit changed();
}
void Document::undo() {
    if (past.isEmpty())
        return;
    future.append(current);
    current = past.takeLast();
    emit changed();
}
void Document::redo() {
    if (future.isEmpty())
        return;
    past.append(current);
    current = future.takeLast();
    emit changed();
}
void Document::paintAnnotation(QPainter &p, const QImage &image, const Annotation &a) {
    if (a.points.isEmpty())
        return;
    auto first = a.points.first(), last = a.points.last();
    QRectF rect(first, last);
    rect = rect.normalized();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(a.opacity);
    p.setPen(QPen(a.color, a.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (a.tool) {
    case Tool::Rectangle:
        p.drawRect(rect);
        break;
    case Tool::Ellipse:
        p.drawEllipse(rect);
        break;
    case Tool::Line:
    case Tool::Arrow: {
        p.drawLine(first, last);
        if (a.tool == Tool::Arrow) {
            double angle = std::atan2(last.y() - first.y(), last.x() - first.x()),
                   len = qMax(10., a.width * 4);
            QPolygonF head;
            head << last << last - QPointF(std::cos(angle - .5) * len, std::sin(angle - .5) * len)
                 << last - QPointF(std::cos(angle + .5) * len, std::sin(angle + .5) * len);
            p.setBrush(a.color);
            p.drawPolygon(head);
        }
        break;
    }
    case Tool::Pen:
    case Tool::Highlighter:
        if (a.tool == Tool::Highlighter) {
            p.setOpacity(a.opacity * .35);
            p.setPen(QPen(a.color, qMax(12., a.width * 4), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
        p.drawPolyline(a.points);
        break;
    case Tool::Text: {
        auto f = a.font;
        if (f.pixelSize() < 1)
            f.setPixelSize(24);
        p.setFont(f);
        p.drawText(QRectF(first, QSizeF(qMax(100., image.width() - first.x()),
                                        qMax(100., image.height() - first.y()))),
                   Qt::TextWordWrap, a.text);
        break;
    }
    case Tool::Number: {
        p.setBrush(a.color);
        p.drawEllipse(first, 14, 14);
        p.setPen(Qt::white);
        QFont font;
        font.setPixelSize(18);
        font.setBold(true);
        p.setFont(font);
        p.drawText(QRectF(first - QPointF(14, 14), QSizeF(28, 28)), Qt::AlignCenter, a.text);
        break;
    }
    case Tool::Mosaic:
    case Tool::Blur: {
        auto r = rect.toAlignedRect().intersected(image.rect());
        p.drawImage(r, redactPixels(image, r, qRound(qMax(6., a.width * 3)), a.tool == Tool::Blur));
        break;
    }
    default:
        break;
    }
    p.restore();
}
QTransform Document::imageTransform() const {
    auto crop = current.crop.intersected(source.rect());
    QTransform rotation;
    rotation.rotate(current.rotation);
    QTransform r = QImage::trueMatrix(rotation, crop.width(), crop.height());
    auto rotated = rotation.mapRect(QRectF(0, 0, crop.width(), crop.height())).size().toSize();
    QTransform flip;
    if (current.flipH) {
        flip.translate(rotated.width(), 0);
        flip.scale(-1, 1);
    }
    if (current.flipV) {
        flip.translate(0, rotated.height());
        flip.scale(1, -1);
    }
    return QTransform::fromTranslate(-crop.x(), -crop.y()) * r * flip;
}
QImage Document::render() const {
    if (source.isNull())
        return {};
    QImage out = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&out);
    if (current.translationMode != 0)
        drawTranslations(p, source, current.translations, current.panel);
    for (const auto &a : current.annotations)
        paintAnnotation(p, source, a);
    p.end();
    if (current.crop != source.rect())
        out = out.copy(current.crop.intersected(source.rect()));
    if (current.rotation % 360)
        out = out.transformed(QTransform().rotate(current.rotation));
    if (current.flipH || current.flipV)
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        out = out.flipped((current.flipH ? Qt::Horizontal : Qt::Orientations()) |
                          (current.flipV ? Qt::Vertical : Qt::Orientations()));
#else
        out = out.mirrored(current.flipH, current.flipV);
#endif
    return out;
}
bool Document::exportImage(QString path, QString &error, QColor bg, int quality) const {
    QElapsedTimer timer;
    timer.start();
    if (current.translationMode != 0)
        for (const auto &u : current.translations) {
            if (rendersTranslation(u) && fitTranslation(u.target, u.box, source.size(), u.fontSize).overflow) {
                error = "部分译文无法完整放入图像。请使用“调整译文”修改区域或字号，或保存工程保留完整文本。";
                return false;
            }
        }
    auto image = render();
    if (image.isNull()) {
        error = "没有可导出的图像";
        return false;
    }
    auto suffix = QFileInfo(path).suffix().toLower();
    QByteArray format = (suffix == "jpg" || suffix == "jpeg") ? "JPEG" : "PNG";
    if (format == "JPEG") {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(bg);
        QPainter p(&flat);
        p.drawImage(0, 0, image);
        p.end();
        image = flat;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    QImageWriter writer(&file, format);
    if (format == "JPEG")
        writer.setQuality(qBound(1, quality, 100));
    else
        // Qt PNG 使用 0..100 的压缩比例，70 对应 zlib 级别 6。
        writer.setCompression(70);
    if (!writer.write(image)) {
        error = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    qInfo("image_export_ms=%lld", timer.elapsed());
    return true;
}
bool Document::saveProject(QString path, QString &error) const {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!source.save(&buffer, "PNG")) {
        error = "原图编码失败";
        return false;
    }
    QJsonArray annotations, ocr, units;
    for (auto a : current.annotations) {
        QJsonArray points;
        for (auto p : a.points)
            points.append(QJsonArray{p.x(), p.y()});
        annotations.append(QJsonObject{{"tool", int(a.tool)},
                                       {"points", points},
                                       {"color", a.color.name(QColor::HexArgb)},
                                       {"width", a.width},
                                       {"opacity", a.opacity},
                                       {"text", a.text},
                                       {"font", a.font.toString()}});
    }
    for (auto l : current.ocr)
        ocr.append(ocrJson(l));
    for (auto u : current.translations)
        units.append(unitJson(u));
    QJsonObject obj{{"format", "SnapLing"},
                    {"version", 1},
                    {"sourcePng", QString::fromLatin1(png.toBase64())},
                    {"annotations", annotations},
                    {"ocr", ocr},
                    {"translations", units},
                    {"crop", rectJson(current.crop)},
                    {"rotation", current.rotation},
                    {"flipH", current.flipH},
                    {"flipV", current.flipV},
                    {"translationMode", current.translationMode},
                    {"panel", current.panel}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    auto bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
bool Document::loadProject(QString path, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.size() > 384 * 1024 * 1024) {
        error = "工程超过 384 MiB 安全限制";
        return false;
    }
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    auto obj = json.object();
    if (parse.error != QJsonParseError::NoError || obj["format"] != "SnapLing" ||
        obj["version"].toInt() != 1) {
        error = "工程已损坏或版本不受支持";
        return false;
    }
    auto bytes = QByteArray::fromBase64(obj["sourcePng"].toString().toLatin1());
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 100000000) {
        error = "工程图像尺寸无效或过大";
        return false;
    }
    auto image = reader.read();
    if (image.isNull()) {
        error = "工程原图损坏";
        return false;
    }
    DocumentState s;
    s.crop = jsonRect(obj["crop"].toObject()).toAlignedRect().intersected(image.rect());
    if (s.crop.isEmpty())
        s.crop = image.rect();
    s.rotation = obj["rotation"].toInt();
    if (s.rotation % 90 != 0) {
        error = "无效旋转角度";
        return false;
    }
    s.flipH = obj["flipH"].toBool();
    s.flipV = obj["flipV"].toBool();
    s.translationMode = qBound(0, obj["translationMode"].toInt(), 2);
    s.panel = obj["panel"].toBool();
    for (auto v : obj["annotations"].toArray()) {
        auto o = v.toObject();
        int tool = o["tool"].toInt();
        if (tool < 0 || tool > int(Tool::Picker)) {
            error = "无效标注类型";
            return false;
        }
        Annotation a;
        a.tool = Tool(tool);
        a.color = QColor(o["color"].toString());
        a.width = qBound(1., o["width"].toDouble(3), 100.);
        a.opacity = qBound(0., o["opacity"].toDouble(1), 1.);
        a.text = o["text"].toString();
        a.font.fromString(o["font"].toString());
        for (auto v : o["points"].toArray()) {
            auto xy = v.toArray();
            if (xy.size() != 2) {
                error = "标注坐标损坏";
                return false;
            }
            double x = xy[0].toDouble(), y = xy[1].toDouble();
            if (!std::isfinite(x) || !std::isfinite(y) || qAbs(x) > 100000 || qAbs(y) > 100000) {
                error = "标注坐标超出范围";
                return false;
            }
            a.points << QPointF(x, y);
        }
        s.annotations << a;
    }
    for (auto v : obj["ocr"].toArray())
        s.ocr << jsonOcr(v.toObject());
    for (auto v : obj["translations"].toArray())
        s.translations << jsonUnit(v.toObject());
    source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    current = s;
    past.clear();
    future.clear();
    emit changed();
    return true;
}
} // namespace SnapLing

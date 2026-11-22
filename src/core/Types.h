#pragma once
#include <QImage>
#include <QJsonObject>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>
namespace SnapLing {
struct CancelToken {
    std::atomic_bool cancelled{false};
    std::atomic_int progress{0};
    void cancel() {
        cancelled.store(true);
    }
    bool isCancelled() const {
        return cancelled.load();
    }
};
using Cancellation = std::shared_ptr<CancelToken>;
enum class CaptureMode { Region, FullScreen, Window, Ellipse, Polygon, Freehand };
struct WindowInfo {
    QRectF rect;
    QString title;
    quintptr id = 0;
};
struct CaptureScreen {
    QString name;
    QRectF desktop; // 此屏幕在全局逻辑坐标中的矩形。
    QImage image;   // 此屏幕的原生像素，独立保留以免即时编辑时重采样。
    qreal devicePixelRatio = 1;
};
struct CaptureFrame {
    QImage image;
    QRectF desktop; // 全局逻辑坐标，像素/逻辑比例按实际图像计算。
    QVector<WindowInfo> windows;
    QVector<CaptureScreen> screens;
    QString screenName;
    qint64 captureMs = 0;
    bool selectionComplete = false; // 已有完整选区；多屏全屏图也用此标记按比例预览。
    QRectF selectionViewport; // 已由系统框选时，裁图在目标屏幕中的局部逻辑坐标。
    bool hasSelectionViewport() const;
    QPointF toPixel(QPointF global) const;
    QRect pixelRect(QRectF global) const;
};
struct OcrLine {
    QString id, text;
    QRectF box;
    QPolygonF polygon;
    double confidence = 0;
    int order = 0, paragraph = 0, direction = 0;
    bool estimatedCharacters = true;
};
struct TranslationUnit {
    QString id, source, target, language, status = "pending";
    QRectF box;
    int fontSize = 0;
    bool overflow = false;
};
QJsonObject rectJson(QRectF r);
QRectF jsonRect(const QJsonObject &o);
QJsonObject ocrJson(const OcrLine &line);
OcrLine jsonOcr(const QJsonObject &o);
QJsonObject unitJson(const TranslationUnit &unit);
TranslationUnit jsonUnit(const QJsonObject &o);
} // namespace SnapLing
Q_DECLARE_METATYPE(SnapLing::CaptureFrame)
Q_DECLARE_METATYPE(SnapLing::OcrLine)
Q_DECLARE_METATYPE(QVector<SnapLing::OcrLine>)

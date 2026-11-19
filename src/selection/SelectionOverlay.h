#pragma once
#include "core/Types.h"
#include <QPainterPath>
#include <QWidget>
namespace SnapLing {
QImage maskedSelection(const QImage &source, const QPainterPath &path);
class SelectionOverlay final : public QWidget {
    Q_OBJECT
  public:
    SelectionOverlay(CaptureFrame frame, CaptureMode mode, QWidget *parent = nullptr);
    QRectF selection() const {
        return selected;
    }
    void confirm();
  signals:
    void accepted(QImage image);
    void cancelled();
    void ready();

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    QPointF imagePoint(QPointF p) const;
    QPainterPath path() const;
    CaptureFrame frame;
    CaptureMode mode;
    QRectF selected, startRect;
    QPointF anchor, cursor;
    QPolygonF points;
    bool dragging = false, moving = false, resizing = false;
    int zoom = 6;
    bool painted = false;
};
} // namespace SnapLing

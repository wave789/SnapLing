#include "SelectionOverlay.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>
#include <QtMath>
namespace SnapLing {
QImage maskedSelection(const QImage &source, const QPainterPath &path) {
    QRect bounds = path.boundingRect().toAlignedRect().intersected(source.rect());
    if (bounds.isEmpty())
        return {};
    QImage result(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-bounds.topLeft());
    p.setClipPath(path);
    p.drawImage(0, 0, source);
    return result;
}
SelectionOverlay::SelectionOverlay(CaptureFrame f, CaptureMode m, QWidget *parent)
    : QWidget(parent), frame(std::move(f)), mode(m) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    if (QGuiApplication::platformName().contains("wayland")) {
        setGeometry(QGuiApplication::primaryScreen()->geometry());
        showFullScreen();
    } else {
        setGeometry(frame.desktop.toAlignedRect());
        show();
    }
    activateWindow();
    setFocus();
    if (mode == CaptureMode::FullScreen)
        selected = frame.image.rect();
}
QPointF SelectionOverlay::imagePoint(QPointF p) const {
    return {qBound(0., p.x() * frame.image.width() / width(), double(frame.image.width())),
            qBound(0., p.y() * frame.image.height() / height(), double(frame.image.height()))};
}
QPainterPath SelectionOverlay::path() const {
    QPainterPath p;
    if (mode == CaptureMode::Ellipse)
        p.addEllipse(selected);
    else if ((mode == CaptureMode::Polygon || mode == CaptureMode::Freehand) && points.size() > 2) {
        p.addPolygon(points);
        p.closeSubpath();
    } else
        p.addRect(selected);
    return p;
}
void SelectionOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.drawImage(rect(), frame.image);
    p.fillRect(rect(), QColor(0, 0, 0, 115));
    p.save();
    p.scale(qreal(width()) / frame.image.width(), qreal(height()) / frame.image.height());
    p.setClipPath(path());
    p.drawImage(0, 0, frame.image);
    p.restore();
    p.save();
    p.scale(qreal(width()) / frame.image.width(), qreal(height()) / frame.image.height());
    p.setPen(QPen(QColor("#60a5fa"), 2. * frame.image.width() / width()));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path());
    p.restore();
    auto sr =
        QRectF(selected.x() * width() / frame.image.width(), selected.y() * height() / frame.image.height(),
               selected.width() * width() / frame.image.width(),
               selected.height() * height() / frame.image.height());
    QString hint = QString("%1 × %2  |  Enter 编辑 · Esc 取消 · 方向键微调 · Shift 调整尺寸")
                       .arg(qRound(selected.width()))
                       .arg(qRound(selected.height()));
    if (mode == CaptureMode::Polygon)
        hint += " · 单击添加顶点，双击闭合";
    QRect label(12, 12, qMin(width() - 24, 720), 30);
    if (!sr.isEmpty())
        label.moveTopLeft(QPoint(qBound(4, int(sr.left()), qMax(4, width() - label.width() - 4)),
                                 qBound(4, int(sr.bottom() + 8), height() - 40)));
    p.fillRect(label, QColor(20, 25, 35, 235));
    p.setPen(Qt::white);
    p.drawText(label.adjusted(8, 0, -8, 0), Qt::AlignVCenter, hint);
    if (!cursor.isNull()) {
        QPoint ip = imagePoint(cursor).toPoint();
        ip.setX(qBound(0, ip.x(), frame.image.width() - 1));
        ip.setY(qBound(0, ip.y(), frame.image.height() - 1));
        QRect loupe(qBound(0, int(cursor.x() + 24), qMax(0, width() - 145)),
                    qBound(0, int(cursor.y() + 30), qMax(0, height() - 150)), 136, 136);
        p.fillRect(loupe, Qt::black);
        p.drawImage(loupe.adjusted(3, 3, -3, -26), frame.image,
                    QRect(ip - QPoint(zoom, zoom), QSize(zoom * 2 + 1, zoom * 2 + 1)));
        p.setPen(Qt::white);
        p.drawText(loupe.adjusted(3, 108, -3, -2), frame.image.pixelColor(ip).name().toUpper());
        p.drawLine(loupe.center().x(), loupe.y() + 3, loupe.center().x(), loupe.bottom() - 26);
    }
    if (!painted) {
        painted = true;
        emit ready();
    }
}
void SelectionOverlay::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        emit cancelled();
        close();
        return;
    }
    cursor = e->position();
    anchor = imagePoint(e->position());
    if (mode == CaptureMode::Polygon) {
        points << anchor;
        selected = points.boundingRect();
        update();
        return;
    }
    startRect = selected;
    resizing = !selected.isEmpty() &&
               QLineF(anchor, selected.bottomRight()).length() < 16. * frame.image.width() / width();
    moving = !resizing && selected.contains(anchor) && mode != CaptureMode::Freehand;
    dragging = true;
    if (mode == CaptureMode::Freehand) {
        points.clear();
        points << anchor;
    }
    if (!moving && !resizing)
        selected = QRectF(anchor, anchor);
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent *e) {
    cursor = e->position();
    auto point = imagePoint(e->position());
    if (dragging) {
        if (moving) {
            selected = startRect.translated(point - anchor);
            if (!points.isEmpty()) {
                auto delta = selected.topLeft() - points.boundingRect().topLeft();
                points.translate(delta);
            }
        } else if (resizing)
            selected = QRectF(startRect.topLeft(), point).normalized();
        else if (mode == CaptureMode::Freehand) {
            points << point;
            selected = points.boundingRect();
        } else
            selected = QRectF(anchor, point).normalized();
        // 邻近图像边界吸附，Alt 可暂时关闭。
        if (!(e->modifiers() & Qt::AltModifier) && !moving && mode != CaptureMode::Freehand) {
            if (selected.left() < 6)
                selected.setLeft(0);
            if (selected.top() < 6)
                selected.setTop(0);
            if (frame.image.width() - selected.right() < 6)
                selected.setRight(frame.image.width());
            if (frame.image.height() - selected.bottom() < 6)
                selected.setBottom(frame.image.height());
        }
    } else if (mode == CaptureMode::Window) {
        for (auto w : frame.windows) {
            auto global = QPointF(frame.desktop.x() + e->position().x() * frame.desktop.width() / width(),
                                  frame.desktop.y() + e->position().y() * frame.desktop.height() / height());
            if (w.rect.contains(global)) {
                selected = frame.pixelRect(w.rect);
                break;
            }
        }
    }
    update();
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent *) {
    dragging = false;
    moving = false;
    resizing = false;
    selected = selected.intersected(frame.image.rect());
    update();
}
void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent *) {
    confirm();
}
void SelectionOverlay::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
        close();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        confirm();
        return;
    }
    QPointF d;
    switch (e->key()) {
    case Qt::Key_Left:
        d = {-1, 0};
        break;
    case Qt::Key_Right:
        d = {1, 0};
        break;
    case Qt::Key_Up:
        d = {0, -1};
        break;
    case Qt::Key_Down:
        d = {0, 1};
        break;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier)
        selected.setBottomRight(selected.bottomRight() + d);
    else {
        selected.translate(d);
        points.translate(d);
    }
    selected = selected.intersected(frame.image.rect());
    update();
}
void SelectionOverlay::wheelEvent(QWheelEvent *e) {
    zoom = qBound(2, zoom + (e->angleDelta().y() > 0 ? -1 : 1), 24);
    update();
}
void SelectionOverlay::confirm() {
    if (selected.width() < 1 || selected.height() < 1)
        return;
    auto result = maskedSelection(frame.image, path());
    if (result.isNull())
        return;
    emit accepted(result);
    close();
}
} // namespace SnapLing

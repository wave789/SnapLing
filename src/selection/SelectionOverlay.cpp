#include "SelectionOverlay.h"
#include "editor/AnnotationTools.h"
#include "editor/Canvas.h"
#include "ui/ToolIcons.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QStyle>
#include <QMenu>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyleHints>
#include <QWindow>
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
SelectionOverlay::SelectionOverlay(CaptureFrame f, CaptureMode m, QWidget *parent,
                                   std::shared_ptr<EditorSession> session, QString activationToken, bool activate)
    : QWidget(parent), frame(std::move(f)),
      anchoredSelection(frame.hasSelectionViewport() && !frame.fullMonitorImage), mode(m),
      editing(std::move(session)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("SnapLing · 即时截图编辑");
    const bool wayland = QGuiApplication::platformName().contains("wayland");
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | (wayland ? Qt::Window : Qt::Tool));
    if (wayland)
        setAttribute(Qt::WA_ShowWithoutActivating);
    if (anchoredSelection)
        setAttribute(Qt::WA_TranslucentBackground);
    else
        setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StaticContents);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    if (wayland) {
        QScreen *screen = nullptr;
        if (!frame.screenName.isEmpty())
            for (auto *candidate : QGuiApplication::screens())
                if (candidate->name() == frame.screenName) {
                    screen = candidate;
                    break;
                }
        if (!screen)
            screen = QGuiApplication::screenAt(frame.desktop.center().toPoint());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        setScreen(screen);
        setGeometry(screen->geometry());
    } else {
        setGeometry(frame.desktop.toAlignedRect());
    }
    if (anchoredSelection) {
        selected = frame.image.rect();
        beginQuickEditing();
    } else if (frame.fullMonitorImage) {
        const auto globalViewport = frame.selectionViewport.translated(frame.desktop.topLeft());
        selected = QRectF(frame.toPixel(globalViewport.topLeft()),
                          frame.toPixel(globalViewport.bottomRight()))
                       .normalized()
                       .intersected(frame.image.rect());
        selectionReady = !selected.isEmpty();
        if (selectionReady) {
            compositorPaintPending = true;
            // Build the selected image and toolbar before the Wayland surface can map.
            prepareQuickEditor();
            QTimer::singleShot(0, this, [this] {
                if (selectionReady && currentPhase == Phase::Selecting) {
                    finishSelection();
                }
            });
        }
    }
    present(std::move(activationToken), activate);
    if (!anchoredSelection && !frame.fullMonitorImage &&
        (mode == CaptureMode::FullScreen || frame.selectionComplete)) {
        selected = frame.image.rect();
        QTimer::singleShot(0, this, &SelectionOverlay::beginQuickEditing);
    }
}
bool SelectionOverlay::matchesPreparedFrame(const CaptureFrame &candidate) const {
    return frame.fullMonitorImage && frame.screenName == candidate.screenName &&
           frame.desktop == candidate.desktop && frame.image.cacheKey() == candidate.image.cacheKey();
}
void SelectionOverlay::completeCompositorSelection(QRectF area) {
    frame.selectionViewport = area;
    frame.selectionComplete = true;
    const auto globalArea = area.translated(frame.desktop.topLeft());
    selected = frame.pixelRect(globalArea);
    compositorPaintPending = true;
    finishSelection();
}
void SelectionOverlay::present(QString token, bool activate) {
    // QtWayland 的 requestActivate 会消费此标准环境变量；只在当前激活请求期间传入。
    const auto previous = qgetenv("XDG_ACTIVATION_TOKEN");
    if (!token.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
    if (QGuiApplication::platformName().contains("wayland"))
        showFullScreen();
    else
        show();
    if (activate) {
        if (QGuiApplication::platformName().contains("wayland")) {
            if (windowHandle())
                windowHandle()->requestActivate();
        } else {
            raise();
            activateWindow();
        }
        if (canvas)
            canvas->setFocus();
        else
            setFocus();
    }
    if (!token.isEmpty()) {
        if (previous.isNull())
            qunsetenv("XDG_ACTIVATION_TOKEN");
        else
            qputenv("XDG_ACTIVATION_TOKEN", previous);
    }
}
void SelectionOverlay::enterEvent(QEnterEvent *) {
    if (currentPhase == Phase::Selecting && !selectionReady && !dragging &&
        (mode == CaptureMode::Region || (mode == CaptureMode::Window && frame.windows.isEmpty()))) {
        selected = frame.image.rect();
        hoverSelection = true;
        update();
    }
}
void SelectionOverlay::leaveEvent(QEvent *) {
    if (currentPhase == Phase::Selecting && !selectionReady && hoverSelection && !dragging) {
        selected = {};
        hoverSelection = false;
        cursor = {};
        update();
    }
}
SelectionOverlay::~SelectionOverlay() {
    if (editing) {
        disconnect(editing.get(), nullptr, this, nullptr);
        disconnect(&editing->document, nullptr, this, nullptr);
    }
    if (canvas) {
        canvas->finishTextEdit(false);
        delete canvas;
        canvas = nullptr;
    }
}
QRectF SelectionOverlay::imageViewport() const {
    if (frame.fullMonitorImage)
        return rect();
    if (anchoredSelection) {
        // On Mutter's physical layout, fullscreen surface geometry can be smaller than the stage coordinates.
        const qreal sx = width() / frame.desktop.width();
        const qreal sy = height() / frame.desktop.height();
        const auto &area = frame.selectionViewport;
        return {area.x() * sx, area.y() * sy, area.width() * sx, area.height() * sy};
    }
    if (!frame.selectionComplete)
        return rect();
    const qreal dpr = devicePixelRatioF();
    QSizeF size(frame.image.width() / dpr, frame.image.height() / dpr);
    const QSizeF available(qMax(1, width() - 40), qMax(1, height() - 180));
    if (size.width() > available.width() || size.height() > available.height())
        size.scale(available, Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}
QRect SelectionOverlay::selectionGeometry() const {
    const auto viewport = imageViewport();
    return QRectF(viewport.x() + selected.x() * viewport.width() / frame.image.width(),
                  viewport.y() + selected.y() * viewport.height() / frame.image.height(),
                  selected.width() * viewport.width() / frame.image.width(),
                  selected.height() * viewport.height() / frame.image.height()).toAlignedRect();
}
QRect SelectionOverlay::loupeGeometry(QPointF point) const {
    return {qBound(0, int(point.x() + 24), qMax(0, width() - 145)),
            qBound(0, int(point.y() + 30), qMax(0, height() - 150)), 136, 136};
}
SelectionOverlay::ResizeHandle SelectionOverlay::handleAt(QPointF point) const {
    if (selected.isEmpty())
        return ResizeHandle::None;
    constexpr qreal tolerance = 10;
    const auto area = QRectF(selectionGeometry());
    const bool left = qAbs(point.x() - area.left()) <= tolerance;
    const bool right = qAbs(point.x() - area.right()) <= tolerance;
    const bool top = qAbs(point.y() - area.top()) <= tolerance;
    const bool bottom = qAbs(point.y() - area.bottom()) <= tolerance;
    const bool withinX = point.x() >= area.left() - tolerance && point.x() <= area.right() + tolerance;
    const bool withinY = point.y() >= area.top() - tolerance && point.y() <= area.bottom() + tolerance;
    if (!withinX || !withinY)
        return ResizeHandle::None;
    if (left && top)
        return ResizeHandle::TopLeft;
    if (right && top)
        return ResizeHandle::TopRight;
    if (left && bottom)
        return ResizeHandle::BottomLeft;
    if (right && bottom)
        return ResizeHandle::BottomRight;
    if (left)
        return ResizeHandle::Left;
    if (right)
        return ResizeHandle::Right;
    if (top)
        return ResizeHandle::Top;
    if (bottom)
        return ResizeHandle::Bottom;
    return area.contains(point) ? ResizeHandle::Move : ResizeHandle::None;
}
void SelectionOverlay::updateCursor(ResizeHandle handle) {
    switch (handle) {
    case ResizeHandle::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case ResizeHandle::Left:
    case ResizeHandle::Right:
        setCursor(Qt::SizeHorCursor);
        break;
    case ResizeHandle::Top:
    case ResizeHandle::Bottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case ResizeHandle::None:
        setCursor(Qt::CrossCursor);
        break;
    }
}
QPointF SelectionOverlay::imagePoint(QPointF point) const {
    const auto viewport = imageViewport();
    return {qBound(0., (point.x() - viewport.x()) * frame.image.width() / viewport.width(), double(frame.image.width())),
            qBound(0., (point.y() - viewport.y()) * frame.image.height() / viewport.height(), double(frame.image.height()))};
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
    QPainter painter(this);
    const auto viewport = imageViewport();
    const qreal dpr = devicePixelRatioF();
    const auto pixelSize = (viewport.size() * dpr).toSize();
    if (!pixelSize.isEmpty() &&
        (displayPixmap.isNull() || displayPixmapSize != pixelSize ||
         displayPixmapSource != frame.image.cacheKey())) {
        auto scaled = frame.image.scaled(pixelSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        displayPixmap = QPixmap::fromImage(std::move(scaled));
        displayPixmap.setDevicePixelRatio(dpr);
        displayPixmapSize = pixelSize;
        displayPixmapSource = frame.image.cacheKey();
    }
    if (anchoredSelection) {
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), QColor(0, 0, 0, 115));
        painter.drawPixmap(viewport.topLeft(), displayPixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        painter.fillRect(rect(), QColor("#151b25"));
        painter.drawPixmap(viewport.topLeft(), displayPixmap);
        painter.fillRect(rect(), QColor(0, 0, 0, 115));
        painter.save();
        QTransform imageToScreen;
        imageToScreen.translate(viewport.x(), viewport.y());
        imageToScreen.scale(viewport.width() / frame.image.width(),
                            viewport.height() / frame.image.height());
        painter.setClipPath(imageToScreen.map(path()));
        painter.drawPixmap(viewport.topLeft(), displayPixmap);
        painter.restore();
    }
    painter.setPen(QPen(QColor("#0bd18b"), 2));
    const auto selection = selectionGeometry();
    if (!selected.isEmpty()) {
        const auto border = QRectF(selection).adjusted(-1, -1, 1, 1)
                                .intersected(QRectF(rect()).adjusted(1, 1, -1, -1));
        painter.drawRect(border);
        if (selectionReady) {
            const auto area = QRectF(selection);
            const QPointF handles[]{
                area.topLeft(), {area.center().x(), area.top()}, area.topRight(),
                {area.left(), area.center().y()}, {area.right(), area.center().y()},
                area.bottomLeft(), {area.center().x(), area.bottom()}, area.bottomRight()};
            painter.setPen(QPen(QColor("#123c30"), 1));
            painter.setBrush(QColor("#0bd18b"));
            for (const auto &point : handles)
                painter.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
            painter.setBrush(Qt::NoBrush);
        }
    }
    if (currentPhase == Phase::Selecting) {
        const QString hint =
            selectionReady
                ? QString("%1 × %2 · 拖动框移动 · 拖动边缘/角点调整 · Enter 或 ✅ 进入编辑 · Esc 取消")
                      .arg(qRound(selected.width())).arg(qRound(selected.height()))
                : QString("%1 × %2 · 单击选整屏，拖动选区域 · 松开后可调整 · Esc 取消")
                      .arg(qRound(selected.width())).arg(qRound(selected.height()));
        QRect label(12, 12, qMax(1, qMin(width() - 24, 760)), 32);
        painter.fillRect(label, QColor(20, 25, 35, 235));
        painter.setPen(Qt::white);
        painter.drawText(label.adjusted(8, 0, -8, 0), Qt::AlignVCenter, hint);
        if (!selectionReady && !cursor.isNull()) {
            auto pixel = imagePoint(cursor).toPoint();
            pixel.setX(qBound(0, pixel.x(), frame.image.width() - 1));
            pixel.setY(qBound(0, pixel.y(), frame.image.height() - 1));
            QRect loupe = loupeGeometry(cursor);
            painter.fillRect(loupe, Qt::black);
            painter.drawImage(loupe.adjusted(3, 3, -3, -26), frame.image,
                              QRect(pixel - QPoint(zoom, zoom), QSize(zoom * 2 + 1, zoom * 2 + 1)));
            painter.setPen(Qt::white);
            painter.drawText(loupe.adjusted(3, 108, -3, -2), frame.image.pixelColor(pixel).name().toUpper());
        }
    }
    if (!painted) {
        painted = true;
        emit ready();
    }
    if (compositorPaintPending) {
        compositorPaintPending = false;
        // Let the backing store commit the completed selection before releasing the Shell preview.
        QTimer::singleShot(0, this, &SelectionOverlay::compositorFrameReady);
    }
}
void SelectionOverlay::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        close();
        return;
    }
    if (currentPhase != Phase::Selecting)
        return;
    if (e->button() != Qt::LeftButton)
        return;
    cursor = e->position();
    pressPosition = e->position();
    anchor = imagePoint(e->position());
    adjustingSelection = selectionReady;
    if (adjustingSelection) {
        resizeHandle = handleAt(e->position());
        startRect = selected;
        moving = resizeHandle == ResizeHandle::Move;
        resizing = resizeHandle != ResizeHandle::None && !moving;
        dragging = resizeHandle != ResizeHandle::None;
        if (dragging) {
            update();
            return;
        }
        // Clicking outside an established selection starts a new box.
        selectionReady = false;
        adjustingSelection = false;
        hoverSelection = false;
        selected = {};
    }
    if (mode == CaptureMode::Polygon) {
        points << anchor;
        selected = points.boundingRect();
        update();
        return;
    }
    if (selected.isEmpty() && (mode == CaptureMode::Region || mode == CaptureMode::Window)) {
        selected = frame.image.rect();
        hoverSelection = true;
    }
    startRect = selected;
    resizing = false;
    moving = false;
    resizeHandle = ResizeHandle::None;
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
    if (currentPhase != Phase::Selecting)
        return;
    const auto previousCursor = cursor;
    const bool hadCursor = !cursor.isNull();
    const auto previousSelection = selectionGeometry();
    cursor = e->position();
    auto point = imagePoint(e->position());
    if (dragging) {
        const auto before = selectionGeometry();
        updateDrag(point, e->modifiers());
        if (selectionReady || adjustingSelection)
            updateCursor(resizeHandle);
        if (selectionReady || adjustingSelection) {
            const int padding = 16;
            QRect dirty = before.adjusted(-padding, -padding, padding, padding)
                              .united(selectionGeometry().adjusted(-padding, -padding, padding, padding));
            dirty = dirty.united(QRect(12, 12, qMax(1, qMin(width() - 24, 760)), 32));
            update(dirty.intersected(rect()));
            return;
        }
    } else if (selectionReady) {
        resizeHandle = handleAt(e->position());
        updateCursor(resizeHandle);
        return;
    } else if (mode == CaptureMode::Region || (mode == CaptureMode::Window && frame.windows.isEmpty())) {
        const bool wasHovering = hoverSelection;
        selected = frame.image.rect();
        hoverSelection = true;
        if (hadCursor && wasHovering) {
            QRect dirty = loupeGeometry(previousCursor).united(loupeGeometry(cursor)).adjusted(-2, -2, 2, 2);
            update(dirty.intersected(rect()));
            return;
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
    QRect dirty = previousSelection.adjusted(-8, -8, 8, 8)
                      .united(selectionGeometry().adjusted(-8, -8, 8, 8))
                      .united(QRect(12, 12, qMax(1, qMin(width() - 24, 760)), 32));
    if (hadCursor)
        dirty = dirty.united(loupeGeometry(previousCursor)).united(loupeGeometry(cursor));
    else
        dirty = rect();
    update(dirty.intersected(rect()));
}
void SelectionOverlay::updateDrag(QPointF point, Qt::KeyboardModifiers modifiers) {
    if (moving) {
        selected = startRect.translated(point - anchor);
        QPointF correction;
        if (selected.left() < 0)
            correction.setX(-selected.left());
        else if (selected.right() > frame.image.width())
            correction.setX(frame.image.width() - selected.right());
        if (selected.top() < 0)
            correction.setY(-selected.top());
        else if (selected.bottom() > frame.image.height())
            correction.setY(frame.image.height() - selected.bottom());
        selected.translate(correction);
        if (!points.isEmpty()) {
            auto delta = selected.topLeft() - points.boundingRect().topLeft();
            points.translate(delta);
        }
    } else if (resizing) {
        auto rect = startRect;
        if (resizeHandle == ResizeHandle::Left || resizeHandle == ResizeHandle::TopLeft ||
            resizeHandle == ResizeHandle::BottomLeft)
            rect.setLeft(point.x());
        if (resizeHandle == ResizeHandle::Right || resizeHandle == ResizeHandle::TopRight ||
            resizeHandle == ResizeHandle::BottomRight)
            rect.setRight(point.x());
        if (resizeHandle == ResizeHandle::Top || resizeHandle == ResizeHandle::TopLeft ||
            resizeHandle == ResizeHandle::TopRight)
            rect.setTop(point.y());
        if (resizeHandle == ResizeHandle::Bottom || resizeHandle == ResizeHandle::BottomLeft ||
            resizeHandle == ResizeHandle::BottomRight)
            rect.setBottom(point.y());
        selected = rect.normalized();
    }
    else if (mode == CaptureMode::Freehand) {
        points << point;
        selected = points.boundingRect();
    } else
        selected = QRectF(anchor, point).normalized();
    // 邻近图像边界吸附，Alt 可暂时关闭。
    if (!(modifiers & Qt::AltModifier) && !moving && !resizing && mode != CaptureMode::Freehand) {
        if (selected.left() < 6)
            selected.setLeft(0);
        if (selected.top() < 6)
            selected.setTop(0);
        if (frame.image.width() - selected.right() < 6)
            selected.setRight(frame.image.width());
        if (frame.image.height() - selected.bottom() < 6)
            selected.setBottom(frame.image.height());
    }
    if (toolbar)
        positionEditor();
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent *event) {
    if (currentPhase != Phase::Selecting || event->button() != Qt::LeftButton)
        return;
    cursor = event->position();
    if (dragging && hoverSelection &&
        QLineF(pressPosition, event->position()).length() < QGuiApplication::styleHints()->startDragDistance())
        selected = startRect;
    else if (dragging)
        updateDrag(imagePoint(event->position()), event->modifiers());
    const bool wasAdjusting = adjustingSelection;
    hoverSelection = false;
    dragging = false;
    moving = false;
    resizing = false;
    selected = selected.intersected(frame.image.rect());
    resizeHandle = ResizeHandle::None;
    if (mode == CaptureMode::Freehand) {
        beginQuickEditing();
    } else if (mode != CaptureMode::Polygon) {
        if (wasAdjusting) {
            selectionReady = !selected.isEmpty();
            if (selectionReady)
                prepareQuickEditor();
        } else {
            finishSelection();
        }
        if (selectionReady)
            updateCursor(handleAt(event->position()));
    }
    update();
}
void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent *) {
    if (currentPhase == Phase::Selecting && mode == CaptureMode::Polygon)
        beginQuickEditing();
}
void SelectionOverlay::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (!canvas || !canvas->isEditingText())
            confirm();
        return;
    }
    if (currentPhase != Phase::Selecting) {
        if (e->matches(QKeySequence::Undo))
            editing->document.undo();
        else if (e->matches(QKeySequence::Redo))
            editing->document.redo();
        else if (e->matches(QKeySequence::Copy))
            copy();
        else
            QWidget::keyPressEvent(e);
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
void SelectionOverlay::finishSelection() {
    if (selected.width() < 1 || selected.height() < 1)
        return;
    hoverSelection = false;
    selectionReady = true;
    prepareQuickEditor();
    if (!selectionNotified) {
        selectionNotified = true;
        emit selectionFinalized();
    }
    updateCursor(handleAt(cursor));
    update();
}
void SelectionOverlay::prepareQuickEditor() {
    if (currentPhase != Phase::Selecting || selected.width() < 1 || selected.height() < 1)
        return;
    if (!editing || preparedSelection != selected) {
        auto image = anchoredSelection ? frame.image : maskedSelection(frame.image, path());
        if (image.isNull())
            return;
        if (!editing)
            editing = std::make_shared<EditorSession>(image);
        else
            editing->document.reset(image);
        preparedSelection = selected;
    }
    if (!canvas) {
        canvas = new Canvas(&editing->document, this);
        canvas->setObjectName("quickCanvas");
        canvas->setOverlayMode(true);
        selectionBorder = new QFrame(this);
        selectionBorder->setObjectName("selectionGreenBorder");
        selectionBorder->setAttribute(Qt::WA_TransparentForMouseEvents);
        selectionBorder->setAttribute(Qt::WA_NoSystemBackground);
        selectionBorder->setStyleSheet("QFrame#selectionGreenBorder{background:transparent;"
                                       "border:2px solid #0bd18b}");
        connect(canvas, &Canvas::confirmRequested, this, &SelectionOverlay::confirm);
        connect(canvas, &Canvas::copyRequested, this, &SelectionOverlay::copy);
        connect(canvas, &Canvas::toolChanged, this, [this](Tool) {
            if (currentPhase == Phase::Selecting)
                showQuickCanvas();
        });
    }
    if (!toolbar)
        createToolbar();
    canvas->hide();
    positionEditor();
    toolbar->show();
    toolbar->raise();
}
void SelectionOverlay::showQuickCanvas() {
    if (currentPhase != Phase::Selecting || !canvas)
        return;
    selectionReady = false;
    currentPhase = Phase::QuickEditing;
    canvas->show();
    positionEditor();
    toolbar->show();
    toolbar->raise();
    selectionBorder->raise();
    toolbar->raise();
    if (auto *confirm = toolbar->findChild<QAction *>("quickConfirmAction"))
        confirm->setToolTip("进入高级编辑器（Enter）");
    canvas->setFocus();
    update();
    if (!quickEditingNotified) {
        quickEditingNotified = true;
        emit quickEditingStarted();
    }
}
void SelectionOverlay::beginQuickEditing() {
    if (currentPhase != Phase::Selecting)
        return;
    prepareQuickEditor();
    showQuickCanvas();
}
void SelectionOverlay::createToolbar() {
    toolbar = new QFrame(this);
    toolbar->setObjectName("quickToolbar");
    toolbar->setStyleSheet("QWidget{background:#202328;color:#d3d9e1}"
                          "QFrame#quickToolbar{border:1px solid #3d444e;border-radius:9px}"
                          "QMenu{background:#202328;border:1px solid #4b5563;padding:5px}"
                          "QMenu::item{padding:7px 20px} QMenu::item:selected{background:#34443f}");
    auto *layout = new QVBoxLayout(toolbar);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *row = new QBoxLayout(QBoxLayout::LeftToRight);
    row->setObjectName("quickToolRow");
    row->setSpacing(0);
    layout->addLayout(row);
    auto *tools = new QToolBar(toolbar);
    tools->setObjectName("quickAnnotationTools");
    addAnnotationTools(tools, canvas, false, true);
    row->addWidget(tools);
    auto *actions = new QToolBar(toolbar);
    actions->setObjectName("quickActionTools");
    row->addWidget(actions);
    auto add = [&](QString name, ToolIcon icon, auto callback) {
        auto *action = actions->addAction(toolIcon(icon, true), name);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    auto *undo = add("撤销", ToolIcon::Undo, [this] { editing->document.undo(); });
    auto *redo = add("重做", ToolIcon::Redo, [this] { editing->document.redo(); });
    undo->setEnabled(false);
    redo->setEnabled(false);
    actions->addSeparator();
    add("OCR", ToolIcon::Ocr, [this] { runOcr(); });
    auto *translation = add("AI 翻译", ToolIcon::Translate, [this] { translate(); });
    translation->setCheckable(true);
    translation->setObjectName("translationToggle");
    auto *translationButton = qobject_cast<QToolButton *>(actions->widgetForAction(translation));
    translationButton->setObjectName("translationButton");
    auto updateTranslation = [translation, translationButton](bool enabled) {
        translation->setChecked(enabled);
        translation->setToolTip(enabled ? "取消翻译，恢复原图" : "AI 翻译");
        translationButton->setToolTip(translation->toolTip());
        translationButton->setProperty("translationActive", enabled);
        translationButton->style()->unpolish(translationButton);
        translationButton->style()->polish(translationButton);
        translationButton->update();
    };
    connect(editing.get(), &EditorSession::translationStateChanged, this, updateTranslation);
    connect(translation, &QAction::triggered, this, [this, updateTranslation] {
        updateTranslation(editing->isTranslationEnabled());
    });
    updateTranslation(editing->isTranslationEnabled());
    auto *cancelTask = add("取消任务", ToolIcon::Stop, [this] { editing->cancel(); });
    cancelTask->setVisible(false);
    actions->addSeparator();
    add("复制图片", ToolIcon::Copy, [this] { canvas->finishTextEdit(true); editing->copyImage(); });
    add("保存", ToolIcon::Save, [this] { canvas->finishTextEdit(true); editing->requestSave(this); });
    add("❌", ToolIcon::Cancel, [this] { close(); })->setToolTip("取消截图（Esc）");
    auto *confirmAction = add("✅", ToolIcon::Confirm, [this] {
        if (currentPhase == Phase::Selecting)
            beginQuickEditing();
        // The confirmation button means "open advanced editing". Do not require
        // a second click when the selection stage has just created the quick canvas.
        confirm();
    });
    confirmAction->setObjectName("quickConfirmAction");
    confirmAction->setToolTip("进入即时编辑（Enter）");
    configureIconToolbar(actions, true);
    auto *approval = new QWidget(toolbar);
    auto *approvalLayout = new QHBoxLayout(approval);
    auto *destination = new QLabel(approval);
    destination->setWordWrap(true);
    auto *allow = new QPushButton("允许发送", approval);
    auto *deny = new QPushButton("取消发送", approval);
    approvalLayout->addWidget(destination, 1);
    approvalLayout->addWidget(allow);
    approvalLayout->addWidget(deny);
    layout->addWidget(approval);
    approval->hide();
    connect(allow, &QPushButton::clicked, editing.get(), [session = editing] { session->approveTranslation(true); });
    connect(deny, &QPushButton::clicked, editing.get(), [session = editing] { session->approveTranslation(false); });
    connect(editing.get(), &EditorSession::translationApprovalRequested, this,
            [this, approval, destination](QString endpoint) {
                destination->setText("将 OCR 文字发送到：" + endpoint);
                approval->show();
                fitToolbar();
                positionEditor();
            });
    connect(editing.get(), &EditorSession::taskChanged, this, [this, cancelTask, approval] {
        cancelTask->setVisible(editing->task() != EditorSession::Task::Idle || editing->awaitingApproval());
        approval->setVisible(editing->awaitingApproval());
        fitToolbar();
        positionEditor();
    });
    statusLabel = new QLabel(toolbar);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);
    statusLabel->hide();
    connect(editing.get(), &EditorSession::statusChanged, this, [this](QString message, bool error) {
        statusLabel->setText(message);
        statusLabel->setStyleSheet(error ? "color:#ff98a3" : "color:#c4cbd4");
        statusLabel->show();
        fitToolbar();
        positionEditor();
        if (!error)
            QTimer::singleShot(5000, this, [this, message] {
                if (statusLabel->text() == message && editing->task() == EditorSession::Task::Idle &&
                    !editing->awaitingApproval()) {
                    statusLabel->hide();
                    fitToolbar();
                    positionEditor();
                }
            });
    });
    connect(canvas, &Canvas::status, this, [this](QString message) {
        statusLabel->setText(message);
        statusLabel->show();
        fitToolbar();
        positionEditor();
    });
    connect(&editing->document, &Document::changed, this, [this, undo, redo] {
        undo->setEnabled(editing->document.canUndo());
        redo->setEnabled(editing->document.canRedo());
        positionEditor();
    });
    fitToolbar();
}
void SelectionOverlay::fitToolbar() {
    if (!toolbar)
        return;
    auto *row = toolbar->findChild<QBoxLayout *>("quickToolRow");
    auto *tools = toolbar->findChild<QToolBar *>("quickAnnotationTools");
    auto *actions = toolbar->findChild<QToolBar *>("quickActionTools");
    const int available = qMax(1, width() - 16);
    const auto layoutMargins = toolbar->layout()->contentsMargins();
    const auto frameMargins = toolbar->contentsMargins();
    const int surround = layoutMargins.left() + layoutMargins.right() + frameMargins.left() + frameMargins.right() + 2;
    tools->setMinimumWidth(qMin(tools->sizeHint().width(), qMax(1, available - surround)));
    actions->setMinimumWidth(qMin(actions->sizeHint().width(), qMax(1, available - surround)));
    const int combined = tools->sizeHint().width() + actions->sizeHint().width() + surround;
    row->setDirection(combined <= available ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    const int desired = combined <= available ? combined : qMax(tools->sizeHint().width(), actions->sizeHint().width()) + surround;
    toolbar->setFixedWidth(qMin(available, desired));
    toolbar->adjustSize();
    toolbar->layout()->activate();
}
void SelectionOverlay::positionEditor() {
    if (!canvas || !toolbar)
        return;
    const auto selection = selectionGeometry();
    canvas->setGeometry(selection);
    if (selectionBorder) {
        const auto border = selection.adjusted(1, 1, -1, -1).intersected(rect());
        selectionBorder->setGeometry(border);
        selectionBorder->setVisible(currentPhase == Phase::QuickEditing);
        if (selectionBorder->isVisible())
            selectionBorder->raise();
    }
    if (toolbar->width() <= 0 || toolbar->height() <= 0)
        fitToolbar();
    const int x = qBound(8, selection.left(), qMax(8, width() - toolbar->width() - 8));
    int y = selection.bottom() + 8;
    if (y + toolbar->height() > height() - 8)
        y = selection.top() - toolbar->height() - 8;
    if (y < 8)
        y = selection.bottom() - toolbar->height() - 8;
    y = qBound(8, y, qMax(8, height() - toolbar->height() - 8));
    toolbar->move(x, y);
    toolbar->raise();
}
void SelectionOverlay::runOcr(bool thenTranslate) {
    if (!canvas)
        return;
    canvas->setTool(Tool::Ocr);
    canvas->setFocus();
    editing->runOcr(thenTranslate);
}
void SelectionOverlay::translate() {
    if (canvas) {
        if (currentPhase == Phase::Selecting)
            showQuickCanvas();
        editing->toggleTranslation(canvas->selectedLines());
    }
}
void SelectionOverlay::copy() {
    const auto selected = canvas->selectedLines();
    if (selected.isEmpty()) {
        editing->copyImage();
        return;
    }
    if (editing->isTranslationEnabled()) {
        QHash<QString, QString> translated;
        for (const auto &unit : editing->document.state().translations)
            if (unit.status == "done")
                translated.insert(unit.id, unit.target);
        QStringList text;
        for (const auto &line : selected)
            if (translated.contains(line.id))
                text << translated.value(line.id);
        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text.join('\n'));
            return;
        }
    }
    QApplication::clipboard()->setText(canvas->selectedText());
}
void SelectionOverlay::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    fitToolbar();
    positionEditor();
}
void SelectionOverlay::closeEvent(QCloseEvent *event) {
    if (!finalized) {
        finalized = true;
        currentPhase = Phase::Cancelled;
        if (canvas)
            canvas->finishTextEdit(false);
        if (editing)
            editing->cancel();
        emit cancelled();
    }
    QWidget::closeEvent(event);
}
void SelectionOverlay::confirm() {
    if (currentPhase == Phase::Selecting) {
        beginQuickEditing();
        return;
    }
    if (currentPhase != Phase::QuickEditing)
        return;
    canvas->finishTextEdit(true);
    currentPhase = Phase::AdvancedEditing;
    finalized = true;
    emit advancedRequested(editing);
    emit accepted(editing->document.render());
    close();
}
} // namespace SnapLing

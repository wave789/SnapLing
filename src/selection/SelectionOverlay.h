#pragma once
#include "core/Types.h"
#include "editor/EditorSession.h"
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>
class QFrame;
class QLabel;
class QComboBox;
namespace SnapLing {
class Canvas;
QImage maskedSelection(const QImage &source, const QPainterPath &path);
class SelectionOverlay final : public QWidget {
    Q_OBJECT
  public:
    SelectionOverlay(CaptureFrame frame, CaptureMode mode, QWidget *parent = nullptr,
                     std::shared_ptr<EditorSession> session = {}, QString activationToken = {}, bool activate = true);
    ~SelectionOverlay() override;
    enum class Phase { Selecting, QuickEditing, AdvancedEditing, Cancelled };
    Phase phase() const { return currentPhase; }
    bool isSelectionReady() const { return selectionReady; }
    Canvas *canvasWidget() const { return canvas; }
    std::shared_ptr<EditorSession> session() const { return editing; }
    QString captureScreenName() const { return frame.screenName; }
    void runOcr(bool thenTranslate = false);
    void translate();
    QRectF selection() const {
        return selected;
    }
    QRect selectionWidgetRect() const {
        return selectionGeometry();
    }
    void confirm();
    void present(QString activationToken = {}, bool activate = true);
  signals:
    void accepted(QImage image);
    void cancelled();
    void ready();
    void selectionFinalized();
    void quickEditingStarted();
    void advancedRequested(std::shared_ptr<SnapLing::EditorSession> session);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

  private:
    enum class ResizeHandle {
        None,
        Move,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };
    QPointF imagePoint(QPointF p) const;
    QPainterPath path() const;
    QRectF imageViewport() const;
    QRect selectionGeometry() const;
    QRect loupeGeometry(QPointF point) const;
    ResizeHandle handleAt(QPointF point) const;
    void updateCursor(ResizeHandle handle);
    void finishSelection();
    void prepareQuickEditor();
    void showQuickCanvas();
    void beginQuickEditing();
    void createToolbar();
    void fitToolbar();
    void positionEditor();
    void copy();
    void updateDrag(QPointF point, Qt::KeyboardModifiers modifiers);
    CaptureFrame frame;
    const bool anchoredSelection;
    CaptureMode mode;
    QRectF selected, startRect;
    QPointF anchor, cursor;
    QPointF pressPosition;
    QPolygonF points;
    bool dragging = false, moving = false, resizing = false;
    bool selectionReady = false, adjustingSelection = false;
    ResizeHandle resizeHandle = ResizeHandle::None;
    bool hoverSelection = false;
    int zoom = 6;
    bool painted = false;
    Phase currentPhase = Phase::Selecting;
    std::shared_ptr<EditorSession> editing;
    Canvas *canvas = nullptr;
    QFrame *toolbar = nullptr;
    QFrame *selectionBorder = nullptr;
    QLabel *statusLabel = nullptr;
    QPixmap displayPixmap;
    qint64 displayPixmapSource = -1;
    QSize displayPixmapSize;
    QRectF preparedSelection;
    bool selectionNotified = false;
    bool quickEditingNotified = false;
    bool finalized = false;
};
} // namespace SnapLing

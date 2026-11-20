#include "SelectionOverlay.h"
#include "editor/AnnotationTools.h"
#include "editor/Canvas.h"
#include "ui/ToolIcons.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFrame>
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
    : QWidget(parent), frame(std::move(f)), mode(m), editing(std::move(session)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("SnapLing · 即时截图编辑");
    const bool wayland = QGuiApplication::platformName().contains("wayland");
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | (wayland ? Qt::Window : Qt::Tool));
    if (wayland)
        setAttribute(Qt::WA_ShowWithoutActivating);
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
    present(std::move(activationToken), activate);
    if (mode == CaptureMode::FullScreen || frame.selectionComplete) {
        selected = frame.image.rect();
        QTimer::singleShot(0, this, &SelectionOverlay::beginQuickEditing);
    }
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
    if (currentPhase == Phase::Selecting && !dragging &&
        (mode == CaptureMode::Region || (mode == CaptureMode::Window && frame.windows.isEmpty()))) {
        selected = frame.image.rect();
        hoverSelection = true;
        update();
    }
}
void SelectionOverlay::leaveEvent(QEvent *) {
    if (currentPhase == Phase::Selecting && hoverSelection && !dragging) {
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
    painter.fillRect(rect(), QColor("#151b25"));
    painter.drawImage(viewport, frame.image);
    painter.fillRect(rect(), QColor(0, 0, 0, 115));
    painter.save();
    painter.translate(viewport.topLeft());
    painter.scale(viewport.width() / frame.image.width(), viewport.height() / frame.image.height());
    painter.setClipPath(path());
    painter.drawImage(0, 0, frame.image);
    painter.restore();
    painter.setPen(QPen(QColor("#0bd18b"), 2));
    const auto selection = selectionGeometry();
    if (!selected.isEmpty()) {
        const auto border = QRectF(selection).adjusted(-1, -1, 1, 1)
                                .intersected(QRectF(rect()).adjusted(1, 1, -1, -1));
        painter.drawRect(border);
    }
    if (currentPhase == Phase::Selecting) {
        const QString hint = QString("%1 × %2 · 单击选整屏，拖动选区域 · 松开立即编辑 · Esc 取消")
                                 .arg(qRound(selected.width())).arg(qRound(selected.height()));
        QRect label(12, 12, qMax(1, qMin(width() - 24, 760)), 32);
        painter.fillRect(label, QColor(20, 25, 35, 235));
        painter.setPen(Qt::white);
        painter.drawText(label.adjusted(8, 0, -8, 0), Qt::AlignVCenter, hint);
        if (!cursor.isNull()) {
            auto pixel = imagePoint(cursor).toPoint();
            pixel.setX(qBound(0, pixel.x(), frame.image.width() - 1));
            pixel.setY(qBound(0, pixel.y(), frame.image.height() - 1));
            QRect loupe(qBound(0, int(cursor.x() + 24), qMax(0, width() - 145)),
                        qBound(0, int(cursor.y() + 30), qMax(0, height() - 150)), 136, 136);
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
    resizing = !hoverSelection && !selected.isEmpty() &&
               QLineF(anchor, selected.bottomRight()).length() < 16. * frame.image.width() / width();
    moving = !hoverSelection && !resizing && selected.contains(anchor) && mode != CaptureMode::Freehand;
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
    cursor = e->position();
    auto point = imagePoint(e->position());
    if (dragging) {
        updateDrag(point, e->modifiers());
    } else if (mode == CaptureMode::Region || (mode == CaptureMode::Window && frame.windows.isEmpty())) {
        selected = frame.image.rect();
        hoverSelection = true;
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
void SelectionOverlay::updateDrag(QPointF point, Qt::KeyboardModifiers modifiers) {
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
    if (!(modifiers & Qt::AltModifier) && !moving && mode != CaptureMode::Freehand) {
        if (selected.left() < 6)
            selected.setLeft(0);
        if (selected.top() < 6)
            selected.setTop(0);
        if (frame.image.width() - selected.right() < 6)
            selected.setRight(frame.image.width());
        if (frame.image.height() - selected.bottom() < 6)
            selected.setBottom(frame.image.height());
    }
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent *event) {
    if (currentPhase != Phase::Selecting || event->button() != Qt::LeftButton)
        return;
    if (dragging && hoverSelection &&
        QLineF(pressPosition, event->position()).length() < QGuiApplication::styleHints()->startDragDistance())
        selected = startRect;
    else if (dragging)
        updateDrag(imagePoint(event->position()), event->modifiers());
    hoverSelection = false;
    dragging = false;
    moving = false;
    resizing = false;
    selected = selected.intersected(frame.image.rect());
    if (mode != CaptureMode::Polygon)
        beginQuickEditing();
    update();
}
void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent *) {
    if (currentPhase == Phase::Selecting)
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
void SelectionOverlay::beginQuickEditing() {
    if (currentPhase != Phase::Selecting || selected.width() < 1 || selected.height() < 1)
        return;
    auto image = maskedSelection(frame.image, path());
    if (image.isNull())
        return;
    if (!editing)
        editing = std::make_shared<EditorSession>(image);
    else
        editing->document.reset(image);
    currentPhase = Phase::QuickEditing;
    canvas = new Canvas(&editing->document, this);
    canvas->setObjectName("quickCanvas");
    canvas->setOverlayMode(true);
    connect(canvas, &Canvas::confirmRequested, this, &SelectionOverlay::confirm);
    connect(canvas, &Canvas::copyRequested, this, &SelectionOverlay::copy);
    createToolbar();
    positionEditor();
    canvas->show();
    toolbar->show();
    toolbar->raise();
    canvas->setFocus();
    update();
    emit quickEditingStarted();
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
        auto *action = actions->addAction(toolIcon(icon), name);
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
    add("全选文字", ToolIcon::SelectText, [this] { canvas->selectAllText(); canvas->setFocus(); });
    add("复制文字", ToolIcon::CopyText, [this] {
        TextLayout text;
        text.setLines(editing->document.state().ocr);
        QApplication::clipboard()->setText(canvas->selectedText().isEmpty() ? text.all() : canvas->selectedText());
    });
    add("复制译文", ToolIcon::CopyTranslation, [this] { editing->copyTranslation(); });
    auto *mode = add("原文、译文、对照", ToolIcon::View, [] {});
    auto *viewMenu = new QMenu(toolbar);
    viewMenu->setObjectName("translationViewMenu");
    auto *viewGroup = new QActionGroup(viewMenu);
    const QStringList views{"原文", "译文", "对照"};
    for (int index = 0; index < views.size(); ++index) {
        auto *view = viewMenu->addAction(views[index]);
        view->setCheckable(true);
        view->setData(index);
        viewGroup->addAction(view);
        view->setChecked(index == editing->document.state().translationMode);
        connect(view, &QAction::triggered, this, [this, index] {
            editing->document.change([index](auto &state) { state.translationMode = index; });
        });
    }
    mode->setMenu(viewMenu);
    qobject_cast<QToolButton *>(actions->widgetForAction(mode))->setPopupMode(QToolButton::InstantPopup);
    auto *cancelTask = add("取消任务", ToolIcon::Stop, [this] { editing->cancel(); });
    cancelTask->setVisible(false);
    actions->addSeparator();
    add("复制图片", ToolIcon::Copy, [this] { canvas->finishTextEdit(true); editing->copyImage(); });
    add("保存", ToolIcon::Save, [this] { canvas->finishTextEdit(true); editing->requestSave(this); });
    add("❌", ToolIcon::Cancel, [this] { close(); })->setToolTip("取消截图（Esc）");
    add("✅", ToolIcon::Confirm, [this] { confirm(); })->setToolTip("进入高级编辑器（Enter）");
    configureIconToolbar(actions);
    originalPreview = new QLabel(toolbar);
    originalPreview->setPixmap(QPixmap::fromImage(editing->document.original().scaled(220, 120, Qt::KeepAspectRatio)));
    originalPreview->hide();
    layout->addWidget(originalPreview);
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
                positionEditor();
            });
    connect(editing.get(), &EditorSession::taskChanged, this, [this, cancelTask, approval] {
        cancelTask->setVisible(editing->task() != EditorSession::Task::Idle || editing->awaitingApproval());
        approval->setVisible(editing->awaitingApproval());
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
        positionEditor();
        if (!error)
            QTimer::singleShot(5000, this, [this, message] {
                if (statusLabel->text() == message && editing->task() == EditorSession::Task::Idle &&
                    !editing->awaitingApproval()) {
                    statusLabel->hide();
                    positionEditor();
                }
            });
    });
    connect(canvas, &Canvas::status, this, [this](QString message) {
        statusLabel->setText(message);
        statusLabel->show();
        positionEditor();
    });
    connect(&editing->document, &Document::changed, this, [this, undo, redo, viewGroup] {
        undo->setEnabled(editing->document.canUndo());
        redo->setEnabled(editing->document.canRedo());
        for (auto *view : viewGroup->actions())
            view->setChecked(view->data().toInt() == editing->document.state().translationMode);
        originalPreview->setVisible(editing->document.state().translationMode == 2);
        positionEditor();
    });
}
void SelectionOverlay::positionEditor() {
    if (!canvas || !toolbar)
        return;
    const auto selection = selectionGeometry();
    canvas->setGeometry(selection);
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
    if (canvas)
        editing->toggleTranslation(canvas->selectedLines());
}
void SelectionOverlay::copy() {
    if (!canvas->selectedText().isEmpty())
        QApplication::clipboard()->setText(canvas->selectedText());
    else
        editing->copyImage();
}
void SelectionOverlay::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
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
    if (currentPhase == Phase::Selecting)
        beginQuickEditing();
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

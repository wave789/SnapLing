#pragma once
#include "Document.h"
#include "ocr/TextLayout.h"
#include <QWidget>
#include <QPointer>
class QTextEdit;
namespace SnapLing {
class Canvas final : public QWidget {
    Q_OBJECT
  public:
    explicit Canvas(Document *document, QWidget *parent = nullptr);
    ~Canvas() override;
    void setOverlayMode(bool enabled);
    bool isEditingText() const { return !textEditor.isNull(); }
    void finishTextEdit(bool commit);
    void setTool(Tool tool);
    Tool tool() const {
        return activeTool;
    }
    void setColor(QColor color);
    void setWidth(qreal width);
    void setFont(QFont font);
    void setOpacity(qreal opacity);
    QString selectedText() const {
        return textLayout.selected(textStart, textEnd);
    }
    void selectAllText();
    QVector<OcrLine> selectedLines() const;
    QColor color() const {
        return penColor;
    }
    QSize sizeHint() const override;
    QTransform sourceToWidget() const;
  signals:
    void copyRequested();
    void status(QString message);
    void confirmRequested();
    void toolChanged(SnapLing::Tool tool);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    QPointF sourcePoint(QPointF p) const;
    void rebuild();
    void updateSelected(std::function<void(Annotation &)> fn);
    void beginTextEdit(Annotation annotation, int index = -1);
    Document *document;
    QImage cached;
    TextLayout textLayout;
    Tool activeTool = Tool::Select;
    QColor penColor = QColor("#ef4444");
    qreal penWidth = 3, penOpacity = 1, zoom = 1;
    QFont penFont;
    Annotation pending;
    bool dragging = false, selecting = false;
    int selected = -1;
    QPointF anchor, delta;
    TextPosition textStart, textEnd;
    bool overlayMode = false;
    QPointer<QTextEdit> textEditor;
    Annotation textDraft;
    int textIndex = -1;
};
} // namespace SnapLing

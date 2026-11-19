#include "Canvas.h"
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBoundaryFinder>
#include <QWheelEvent>
namespace SnapLing {
Canvas::Canvas(Document *d, QWidget *p) : QWidget(p), document(d) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    penFont.setPixelSize(24);
    connect(d, &Document::changed, this, &Canvas::rebuild);
    rebuild();
}
void Canvas::rebuild() {
    cached = document->render();
    textLayout.setLines(document->state().ocr);
    if (selected >= document->state().annotations.size())
        selected = -1;
    updateGeometry();
    update();
}
QSize Canvas::sizeHint() const {
    return cached.isNull() ? QSize(900, 550)
                           : cached.size()
                                 .scaled(QSize(qRound(1100 * zoom), qRound(700 * zoom)), Qt::KeepAspectRatio)
                                 .expandedTo(QSize(400, 300));
}
QTransform Canvas::sourceToWidget() const {
    if (cached.isNull())
        return {};
    qreal scale = qMin(qreal(width() - 32) / cached.width(), qreal(height() - 32) / cached.height()) * zoom;
    QTransform t;
    t.translate((width() - cached.width() * scale) / 2, (height() - cached.height() * scale) / 2);
    t.scale(scale, scale);
    return document->imageTransform() * t;
}
QPointF Canvas::sourcePoint(QPointF p) const {
    return sourceToWidget().inverted().map(p);
}
void Canvas::setTool(Tool t) {
    activeTool = t;
    selected = -1;
    dragging = false;
    selecting = false;
    if (t != Tool::Ocr) {
        textStart = {};
        textEnd = {};
    }
    setCursor(t == Tool::Ocr ? Qt::IBeamCursor : t == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    update();
}
void Canvas::updateSelected(std::function<void(Annotation &)> fn) {
    if (selected < 0 || selected >= document->state().annotations.size())
        return;
    document->change([&](auto &s) { fn(s.annotations[selected]); });
}
void Canvas::setColor(QColor c) {
    penColor = c;
    updateSelected([&](auto &a) { a.color = c; });
}
void Canvas::setWidth(qreal w) {
    penWidth = w;
    updateSelected([&](auto &a) { a.width = w; });
}
void Canvas::setFont(QFont f) {
    penFont = f;
    updateSelected([&](auto &a) { a.font = f; });
}
void Canvas::setOpacity(qreal o) {
    penOpacity = o;
    updateSelected([&](auto &a) { a.opacity = o; });
}
QVector<OcrLine> Canvas::selectedLines() const {
    QVector<OcrLine> out;
    auto a = textStart, b = textEnd;
    if (!a.valid() || !b.valid())
        return out;
    if (a > b)
        std::swap(a, b);
    for (int i = a.line; i <= b.line && i < textLayout.lines().size(); ++i) {
        auto line = textLayout.lines()[i];
        auto chars = textLayout.characters(i);
        auto boxes = textLayout.characterBoxes(i);
        int start = i == a.line ? a.index : 0, end = i == b.line ? b.index : int(chars.size()) - 1;
        line.text.clear();
        QRectF box;
        for (int j = qMax(0, start); j <= end && j < chars.size(); ++j) {
            line.text += chars[j];
            box = box.united(boxes[j]);
        }
        if (!line.text.isEmpty()) {
            line.box = box;
            line.polygon = QPolygonF(box);
            out << line;
        }
    }
    return out;
}
void Canvas::selectAllText() {
    if (textLayout.lines().isEmpty())
        return;
    activeTool = Tool::Ocr;
    textStart = {0, 0};
    int last = textLayout.lines().size() - 1;
    textEnd = {last, int(textLayout.characters(last).size()) - 1};
    update();
}
void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#252b36"));
    if (cached.isNull())
        return;
    auto t = sourceToWidget();
    auto view = document->imageTransform().inverted() * t;
    p.setTransform(view);
    p.drawImage(0, 0, cached);
    p.setTransform(t);
    if (activeTool == Tool::Ocr) {
        p.setPen(QPen(QColor(80, 165, 245, 100), 1));
        p.setBrush(Qt::NoBrush);
        for (auto l : textLayout.lines())
            p.drawRect(l.box);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(50, 145, 255, 95));
        for (auto r : textLayout.selectionRects(textStart, textEnd))
            p.drawRect(r);
    }
    if (selected >= 0 && selected < document->state().annotations.size()) {
        auto a = document->state().annotations[selected];
        p.setPen(QPen(QColor("#60a5fa"), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(a.bounds().translated(delta));
        if (dragging) {
            a.points.translate(delta);
            Document::paintAnnotation(p, document->original(), a);
        }
    }
    if (dragging && activeTool != Tool::Select && activeTool != Tool::Ocr) {
        if (activeTool == Tool::Crop) {
            p.setPen(QPen(Qt::cyan, 1, Qt::DashLine));
            p.drawRect(pending.points.boundingRect());
        } else
            Document::paintAnnotation(p, document->original(), pending);
    }
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    setFocus();
    anchor = sourcePoint(e->position());
    delta = {};
    if (!document->original().rect().contains(anchor.toPoint()))
        return;
    if (activeTool == Tool::Picker) {
        auto color = document->original().pixelColor(anchor.toPoint());
        QApplication::clipboard()->setText(color.name().toUpper());
        emit status(QString("已复制 %1 · RGB(%2, %3, %4)")
                        .arg(color.name().toUpper())
                        .arg(color.red())
                        .arg(color.green())
                        .arg(color.blue()));
        return;
    }
    if (activeTool == Tool::Ocr) {
        textStart = textLayout.hit(anchor);
        textEnd = textStart;
        selecting = textStart.valid();
        update();
        return;
    }
    if (activeTool == Tool::Select) {
        selected = -1;
        const auto &annotations = document->state().annotations;
        for (int i = annotations.size() - 1; i >= 0; --i)
            if (annotations[i].bounds().contains(anchor)) {
                selected = i;
                break;
            }
        dragging = selected >= 0;
        update();
        return;
    }
    pending = {};
    pending.tool = activeTool;
    pending.color = penColor;
    pending.width = penWidth;
    pending.opacity = penOpacity;
    pending.font = penFont;
    pending.points << anchor;
    if (activeTool == Tool::Text) {
        bool ok = false;
        pending.text = QInputDialog::getMultiLineText(this, "文字标注", "输入文字", {}, &ok);
        if (ok && !pending.text.isEmpty()) {
            auto a = pending;
            document->change([a](auto &s) { s.annotations << a; });
        }
        return;
    }
    if (activeTool == Tool::Number) {
        pending.text = QString::number(std::count_if(document->state().annotations.begin(),
                                                     document->state().annotations.end(),
                                                     [](auto a) { return a.tool == Tool::Number; }) +
                                       1);
        auto a = pending;
        document->change([a](auto &s) { s.annotations << a; });
        return;
    }
    pending.points << anchor;
    dragging = true;
    update();
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    auto point = sourcePoint(e->position());
    if (selecting) {
        textEnd = textLayout.hit(point, true);
        update();
        return;
    }
    if (!dragging) {
        if (activeTool == Tool::Ocr)
            setCursor(textLayout.hit(point).valid() ? Qt::IBeamCursor : Qt::ArrowCursor);
        return;
    }
    if (activeTool == Tool::Select)
        delta = point - anchor;
    else if (activeTool == Tool::Pen || activeTool == Tool::Highlighter)
        pending.points << point;
    else
        pending.points.last() = point;
    update();
}
void Canvas::mouseReleaseEvent(QMouseEvent *) {
    selecting = false;
    if (!dragging)
        return;
    dragging = false;
    if (activeTool == Tool::Select) {
        auto move = delta;
        delta = {};
        if (!move.isNull() && selected >= 0)
            document->change([&](auto &s) { s.annotations[selected].points.translate(move); });
    } else if (activeTool == Tool::Crop) {
        auto crop = pending.points.boundingRect().toAlignedRect().intersected(document->original().rect());
        if (crop.width() > 1 && crop.height() > 1)
            document->change([crop](auto &s) { s.crop = crop; });
    } else if (pending.points.size() > 1) {
        auto a = pending;
        document->change([a](auto &s) { s.annotations << a; });
    }
    update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    auto pos = sourcePoint(e->position());
    if (activeTool == Tool::Ocr) {
        auto hit = textLayout.hit(pos);
        if (!hit.valid())
            return;
        auto chars = textLayout.characters(hit.line);
        bool whole = e->modifiers() & Qt::ControlModifier;
        int a = hit.index, b = hit.index;
        auto word = [](QString c) { return !c.isEmpty() && (c[0].isLetterOrNumber() || c == "_"); };
        if (whole) {
            a = 0;
            b = chars.size() - 1;
        } else if (word(chars[a])) {
            while (a > 0 && word(chars[a - 1]))
                --a;
            while (b + 1 < chars.size() && word(chars[b + 1]))
                ++b;
        }
        textStart = {hit.line, a};
        textEnd = {hit.line, b};
        update();
    } else if (selected >= 0 && document->state().annotations[selected].tool == Tool::Text) {
        bool ok;
        auto text = QInputDialog::getMultiLineText(this, "修改标注", "文字",
                                                   document->state().annotations[selected].text, &ok);
        if (ok)
            updateSelected([&](auto &a) { a.text = text; });
    }
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (e->matches(QKeySequence::Copy)) {
        emit copyRequested();
        return;
    }
    if (e->matches(QKeySequence::SelectAll) && activeTool == Tool::Ocr) {
        selectAllText();
        return;
    }
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selected >= 0) {
        int index = selected;
        selected = -1;
        document->change([index](auto &s) { s.annotations.removeAt(index); });
        return;
    }
    QWidget::keyPressEvent(e);
}
void Canvas::wheelEvent(QWheelEvent *e) {
    zoom = qBound(.2, zoom * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15), 5.);
    update();
}
} // namespace SnapLing

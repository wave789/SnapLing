#include "ToolIcons.h"
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QToolBar>
#include <QToolButton>

namespace SnapLing {
static void drawIcon(QPainter &p, ToolIcon icon) {
    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) { p.drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    switch (icon) {
    case ToolIcon::Select:
        p.drawPolygon(QPolygonF{{5, 3}, {18, 12}, {12, 13}, {9, 20}});
        break;
    case ToolIcon::Rectangle: p.drawRoundedRect(QRectF(4, 5, 16, 14), 1.5, 1.5); break;
    case ToolIcon::Ellipse: p.drawEllipse(QRectF(4, 4, 16, 16)); break;
    case ToolIcon::Line: line(5, 19, 19, 5); break;
    case ToolIcon::Arrow:
        line(5, 19, 19, 5); p.drawPolyline(QPolygonF{{10, 5}, {19, 5}, {19, 14}}); break;
    case ToolIcon::Pen:
    case ToolIcon::Highlighter:
        p.drawPolygon(QPolygonF{{5, 15}, {15, 5}, {19, 9}, {9, 19}, {4, 20}});
        line(13, 7, 17, 11);
        if (icon == ToolIcon::Highlighter) line(12, 20, 21, 20);
        break;
    case ToolIcon::Text:
        p.drawRoundedRect(QRectF(3.5, 3.5, 17, 17), 2, 2);
        line(8, 8, 16, 8); line(12, 8, 12, 17); line(10, 17, 14, 17); break;
    case ToolIcon::Number:
        p.drawEllipse(QRectF(3, 3, 18, 18));
        p.drawPolyline(QPolygonF{{10, 9}, {12, 7}, {12, 17}}); line(9, 17, 15, 17); break;
    case ToolIcon::Mosaic:
        p.drawRoundedRect(QRectF(4, 4, 16, 16), 1.5, 1.5);
        for (int y = 6; y < 19; y += 4)
            for (int x = 6; x < 19; x += 4)
                if ((x + y) % 8 == 4) p.fillRect(QRectF(x, y, 3, 3), p.pen().color());
        break;
    case ToolIcon::Blur:
        for (int y = 5; y <= 19; y += 7)
            for (int x = 5; x <= 19; x += 7) p.drawEllipse(QPointF(x, y), x == 12 && y == 12 ? 2.4 : 1.1, 1.1);
        break;
    case ToolIcon::Crop:
        p.drawPolyline(QPolygonF{{3, 7}, {17, 7}, {17, 21}});
        p.drawPolyline(QPolygonF{{7, 3}, {7, 17}, {21, 17}}); break;
    case ToolIcon::Picker:
        p.drawPolygon(QPolygonF{{5, 15}, {14, 6}, {18, 10}, {9, 19}, {4, 20}});
        line(12, 4, 20, 12); line(16, 4, 20, 8); break;
    case ToolIcon::Style:
        line(5, 7, 19, 7); line(5, 12, 19, 12); line(5, 17, 19, 17);
        p.drawEllipse(QPointF(10, 7), 2, 2); p.drawEllipse(QPointF(16, 12), 2, 2);
        p.drawEllipse(QPointF(8, 17), 2, 2); break;
    case ToolIcon::Ocr:
    case ToolIcon::SelectText:
        p.drawPolyline(QPolygonF{{8, 4}, {4, 4}, {4, 9}});
        p.drawPolyline(QPolygonF{{16, 4}, {20, 4}, {20, 9}});
        p.drawPolyline(QPolygonF{{4, 15}, {4, 20}, {8, 20}});
        p.drawPolyline(QPolygonF{{20, 15}, {20, 20}, {16, 20}});
        line(8, 9, 16, 9); line(12, 9, 12, 16);
        if (icon == ToolIcon::SelectText) line(8, 18, 16, 18);
        break;
    case ToolIcon::Translate:
    case ToolIcon::CopyTranslation:
        p.drawRoundedRect(QRectF(3, 3, 12, 12), 2, 2);
        p.drawPolyline(QPolygonF{{17, 9}, {21, 9}, {21, 21}, {9, 21}, {9, 17}});
        p.drawPolyline(QPolygonF{{6, 12}, {9, 6}, {12, 12}}); line(7, 10, 11, 10);
        line(14, 14, 18, 14); p.drawPolyline(QPolygonF{{16, 12}, {16, 16}, {18, 18}});
        if (icon == ToolIcon::CopyTranslation) line(12, 19, 14, 17);
        break;
    case ToolIcon::Copy:
    case ToolIcon::CopyText:
        p.drawRoundedRect(QRectF(8, 8, 12, 13), 2, 2);
        p.drawPolyline(QPolygonF{{15, 5}, {15, 3}, {3, 3}, {3, 16}, {5, 16}});
        if (icon == ToolIcon::CopyText) { line(11, 12, 17, 12); line(14, 12, 14, 17); }
        break;
    case ToolIcon::Save:
        p.drawPolyline(QPolygonF{{4, 14}, {4, 20}, {20, 20}, {20, 14}});
        line(12, 3, 12, 15); p.drawPolyline(QPolygonF{{8, 11}, {12, 15}, {16, 11}}); break;
    case ToolIcon::Undo:
    case ToolIcon::Redo: {
        if (icon == ToolIcon::Redo) { p.translate(24, 0); p.scale(-1, 1); }
        QPainterPath path; path.moveTo(5, 9); path.cubicTo(18, 5, 23, 18, 13, 20); p.drawPath(path);
        p.drawPolyline(QPolygonF{{9, 4}, {5, 9}, {11, 12}}); break;
    }
    case ToolIcon::Rotate:
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
        p.drawArc(QRectF(2, 2, 20, 20), 38 * 16, 280 * 16);
        p.drawPolyline(QPolygonF{{18, 2}, {22, 5}, {19, 9}});
        break;
    case ToolIcon::View:
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2); line(12, 5, 12, 19);
        p.drawEllipse(QPointF(7.5, 12), 1.8, 1.8); line(15, 10, 18, 10); line(15, 14, 18, 14); break;
    case ToolIcon::Stop: p.drawRoundedRect(QRectF(6, 6, 12, 12), 2, 2); break;
    case ToolIcon::Confirm: p.drawPolyline(QPolygonF{{5, 12}, {10, 17}, {20, 6}}); break;
    case ToolIcon::Cancel: line(6, 6, 18, 18); line(18, 6, 6, 18); break;
    }
}
QIcon toolIcon(ToolIcon icon) {
    return toolIcon(icon, QApplication::palette().color(QPalette::Window).lightness() < 128);
}
QIcon toolIcon(ToolIcon icon, bool dark) {
    static QHash<int, QIcon> cache;
    const int key = (int(icon) << 1) | int(dark);
    const auto found = cache.constFind(key);
    if (found != cache.cend())
        return found.value();
    QIcon result;
    for (const auto size : {16, 20, 24, 32, 40, 48, 64})
        for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled})
            for (const auto state : {QIcon::Off, QIcon::On}) {
                QPixmap pixels(size, size);
                pixels.fill(Qt::transparent);
                QPainter painter(&pixels);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.scale(size / 24., size / 24.);
                QColor color = dark
                                   ? (mode == QIcon::Disabled ? QColor("#5d646e") : QColor("#bec5cf"))
                                   : (mode == QIcon::Disabled ? QColor("#9ca3af") : QColor("#4b5563"));
                if (mode == QIcon::Active)
                    color = dark ? QColor("#f3f6fa") : QColor("#111827");
                if (state == QIcon::On || icon == ToolIcon::Confirm)
                    color = dark ? QColor("#0bd18b") : QColor("#059669");
                if (state == QIcon::On && icon == ToolIcon::Translate) color = Qt::white;
                if (icon == ToolIcon::Cancel)
                    color = dark ? QColor("#ff6675") : QColor("#dc2626");
                painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                drawIcon(painter, icon);
                painter.end();
                result.addPixmap(pixels, mode, state);
            }
    cache.insert(key, result);
    return result;
}
void configureIconToolbar(QToolBar *toolbar, bool dark) {
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize({20, 20});
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    if (dark)
        toolbar->setStyleSheet(
            "QToolBar{background:#202328;border:0;spacing:1px;padding:0}"
            "QToolButton{background:transparent;border:1px solid transparent;border-radius:5px;padding:5px;color:#d3d9e1}"
            "QToolButton:hover{background:#343a43}"
            "QToolButton:checked{background:#153e32;border-color:#24604b}"
            "QToolButton[translationActive=\"true\"]{background:#08b783;border-color:#08b783}"
            "QToolButton[translationActive=\"true\"]:hover{background:#09c58d}"
            "QToolBar::separator{background:#454b54;width:1px;margin:7px 4px}"
            "QSpinBox,QDoubleSpinBox{background:#2c323b;color:#d3d9e1;border:1px solid #49525e;padding:3px}");
    else
        toolbar->setStyleSheet({});
    for (auto *action : toolbar->actions())
        if (auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(action))) {
            button->setAccessibleName(action->text());
            button->setToolTip(action->toolTip());
        }
}
}

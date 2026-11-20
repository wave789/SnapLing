#include "AnnotationTools.h"
#include "Canvas.h"
#include "ui/ToolIcons.h"
#include <QActionGroup>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFontDialog>
#include <QMenu>
#include <QSpinBox>
#include <QToolBar>
#include <QWidgetAction>
#include <tuple>

namespace SnapLing {
static void addProperties(QToolBar *toolbar, Canvas *canvas) {
    QObject::connect(toolbar->addAction(toolIcon(ToolIcon::Style), "颜色"), &QAction::triggered, canvas, [canvas] {
        const auto color = QColorDialog::getColor(canvas->color(), canvas);
        if (color.isValid())
            canvas->setColor(color);
    });
    auto *width = new QSpinBox(toolbar);
    width->setRange(1, 50);
    width->setValue(3);
    width->setPrefix("线宽 ");
    width->setAccessibleName("线宽");
    toolbar->addWidget(width);
    QObject::connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas, &Canvas::setWidth);
    QObject::connect(toolbar->addAction(toolIcon(ToolIcon::Text), "字体"), &QAction::triggered, canvas, [canvas] {
        bool accepted;
        auto font = QFontDialog::getFont(&accepted, QFont(), canvas);
        if (accepted) {
            if (font.pixelSize() < 1)
                font.setPixelSize(qRound(font.pointSizeF() * 96 / 72));
            canvas->setFont(font);
        }
    });
    auto *opacity = new QDoubleSpinBox(toolbar);
    opacity->setRange(.05, 1);
    opacity->setSingleStep(.1);
    opacity->setValue(1);
    opacity->setPrefix("透明度 ");
    opacity->setAccessibleName("透明度");
    toolbar->addWidget(opacity);
    QObject::connect(opacity, qOverload<double>(&QDoubleSpinBox::valueChanged), canvas, &Canvas::setOpacity);
    configureIconToolbar(toolbar);
}
void addAnnotationTools(QToolBar *toolbar, Canvas *canvas, bool advanced, bool compact) {
    auto *group = new QActionGroup(toolbar);
    QList<std::tuple<QString, Tool, ToolIcon>> tools{
        {"选择", Tool::Select, ToolIcon::Select}, {"矩形", Tool::Rectangle, ToolIcon::Rectangle},
        {"椭圆", Tool::Ellipse, ToolIcon::Ellipse}, {"直线", Tool::Line, ToolIcon::Line},
        {"箭头", Tool::Arrow, ToolIcon::Arrow}, {"画笔", Tool::Pen, ToolIcon::Pen},
        {"荧光", Tool::Highlighter, ToolIcon::Highlighter}, {"文字", Tool::Text, ToolIcon::Text},
        {"序号", Tool::Number, ToolIcon::Number}, {"马赛克", Tool::Mosaic, ToolIcon::Mosaic},
        {"模糊", Tool::Blur, ToolIcon::Blur}};
    if (advanced) {
        tools.append({"裁剪", Tool::Crop, ToolIcon::Crop});
        tools.append({"取色", Tool::Picker, ToolIcon::Picker});
    }
    for (const auto &[name, tool, icon] : tools) {
        auto *action = toolbar->addAction(toolIcon(icon), name);
        action->setCheckable(true);
        action->setChecked(tool == Tool::Select);
        action->setData(int(tool));
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, canvas, [canvas, tool] {
            canvas->setTool(tool);
            canvas->setFocus();
        });
    }
    QObject::connect(canvas, &Canvas::toolChanged, group, [group](Tool tool) {
        group->setExclusive(false);
        for (auto *action : group->actions())
            action->setChecked(action->data().toInt() == int(tool));
        group->setExclusive(true);
    });
    toolbar->addSeparator();
    if (compact) {
        auto *menu = new QMenu(toolbar);
        auto *properties = new QToolBar(menu);
        addProperties(properties, canvas);
        auto *content = new QWidgetAction(menu);
        content->setDefaultWidget(properties);
        menu->addAction(content);
        auto *style = toolbar->addAction(toolIcon(ToolIcon::Style), "颜色、线宽与字体");
        QObject::connect(style, &QAction::triggered, toolbar, [toolbar, style, menu] {
            const auto *button = toolbar->widgetForAction(style);
            menu->popup(button->mapToGlobal(QPoint(0, button->height() + 4)));
        });
    } else
        addProperties(toolbar, canvas);
    configureIconToolbar(toolbar);
}
} // namespace SnapLing

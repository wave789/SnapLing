#include "AnnotationTools.h"
#include "Canvas.h"
#include <QActionGroup>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFontDialog>
#include <QSpinBox>
#include <QToolBar>

namespace SnapLing {
void addAnnotationTools(QToolBar *toolbar, Canvas *canvas, bool advanced) {
    auto *group = new QActionGroup(toolbar);
    QList<QPair<QString, Tool>> tools{
        {"选择", Tool::Select}, {"矩形", Tool::Rectangle}, {"椭圆", Tool::Ellipse}, {"直线", Tool::Line},
        {"箭头", Tool::Arrow}, {"画笔", Tool::Pen}, {"荧光", Tool::Highlighter}, {"文字", Tool::Text},
        {"序号", Tool::Number}, {"马赛克", Tool::Mosaic}, {"模糊", Tool::Blur}};
    if (advanced) {
        tools.append({"裁剪", Tool::Crop});
        tools.append({"取色", Tool::Picker});
    }
    for (const auto &[name, tool] : tools) {
        auto *action = toolbar->addAction(name);
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
    QObject::connect(toolbar->addAction("颜色"), &QAction::triggered, canvas, [canvas] {
        const auto color = QColorDialog::getColor(canvas->color(), canvas);
        if (color.isValid())
            canvas->setColor(color);
    });
    auto *width = new QSpinBox(toolbar);
    width->setRange(1, 50);
    width->setValue(3);
    width->setPrefix("线宽 ");
    toolbar->addWidget(width);
    QObject::connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas, &Canvas::setWidth);
    QObject::connect(toolbar->addAction("字体"), &QAction::triggered, canvas, [canvas] {
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
    toolbar->addWidget(opacity);
    QObject::connect(opacity, qOverload<double>(&QDoubleSpinBox::valueChanged), canvas, &Canvas::setOpacity);
}
} // namespace SnapLing

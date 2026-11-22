#pragma once
#include <QIcon>
class QToolBar;
namespace SnapLing {
enum class ToolIcon {
    Select, Rectangle, Ellipse, Line, Arrow, Pen, Highlighter, Text, Number, Mosaic, Blur,
    Crop, Picker, Style, Ocr, Translate, SelectText, CopyText, CopyTranslation, Copy, Save,
    Undo, Redo, Rotate, View, Stop, Confirm, Cancel
};
// 原创 24×24 线性图标，按 Qt 请求尺寸绘制，包含普通、选中及禁用状态。
QIcon toolIcon(ToolIcon icon);
QIcon toolIcon(ToolIcon icon, bool dark);
void configureIconToolbar(QToolBar *toolbar, bool dark = false);
}

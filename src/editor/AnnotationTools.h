#pragma once
class QToolBar;
namespace SnapLing {
class Canvas;
void addAnnotationTools(QToolBar *toolbar, Canvas *canvas, bool advanced, bool compact = false);
} // namespace SnapLing

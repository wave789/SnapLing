#include "PinWindow.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QWindow>
namespace SnapLing {
PinWindow::PinWindow(QImage i) : image(std::move(i)) {
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("SnapLing 贴图 · 右键菜单");
    resize(image.size().scaled(640, 480, Qt::KeepAspectRatio));
    setMinimumSize(80, 60);
    show();
}
void PinWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), Qt::black);
    p.drawImage(QRect(QPoint(0, 0), image.size().scaled(size(), Qt::KeepAspectRatio)), image);
}
void PinWindow::mousePressEvent(QMouseEvent *e) {
    drag = e->globalPosition().toPoint() - frameGeometry().topLeft();
    if (e->button() == Qt::LeftButton && windowHandle())
        windowHandle()->startSystemMove();
}
void PinWindow::mouseMoveEvent(QMouseEvent *e) {
    if ((e->buttons() & Qt::LeftButton) && !QGuiApplication::platformName().contains("wayland"))
        move(e->globalPosition().toPoint() - drag);
}
void PinWindow::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier)
        setWindowOpacity(qBound(.15, windowOpacity() + (e->angleDelta().y() > 0 ? .05 : -.05), 1.));
    else
        resize(size() * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
}
void PinWindow::contextMenuEvent(QContextMenuEvent *e) {
    QMenu m;
    auto *copy = m.addAction("复制图片"), *save = m.addAction("保存图片…"),
         *top = m.addAction("始终置顶（取决于桌面支持）"), *close = m.addAction("关闭贴图");
    top->setCheckable(true);
    top->setChecked(windowFlags() & Qt::WindowStaysOnTopHint);
    auto *choice = m.exec(e->globalPos());
    if (choice == copy)
        QApplication::clipboard()->setImage(image);
    if (choice == save) {
        auto f = QFileDialog::getSaveFileName(this, "保存贴图", {}, "PNG (*.png)");
        if (!f.isEmpty() && !image.save(f))
            QMessageBox::warning(this, "保存失败", "无法写入图片");
    }
    if (choice == top) {
        setWindowFlag(Qt::WindowStaysOnTopHint, top->isChecked());
        show();
    }
    if (choice == close)
        this->close();
}
} // namespace SnapLing

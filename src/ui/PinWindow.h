#pragma once
#include <QWidget>
namespace SnapLing {
class PinWindow final : public QWidget {
    Q_OBJECT
  public:
    explicit PinWindow(QImage image);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

  private:
    QImage image;
    QPoint drag;
};
} // namespace SnapLing

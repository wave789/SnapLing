#include "CaptureService.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QtMath>
#include <cmath>
#ifdef Q_OS_WIN
#include "platform/windows/WindowsCapture.h"
#else
#include "platform/linux/wayland/WaylandCapture.h"
#include "platform/linux/x11/X11Capture.h"
#endif
namespace SnapLing {
QVector<CaptureScreen> captureScreenLayout() {
    QVector<CaptureScreen> result;
    for (auto *screen : QGuiApplication::screens())
        result << CaptureScreen{screen->name(), screen->geometry(), {}, screen->devicePixelRatio()};
    return result;
}
CaptureFrame combineCaptureScreens(QVector<CaptureScreen> screens, QString &error) {
    error.clear();
    CaptureFrame result;
    if (screens.isEmpty() || screens.size() > 16) {
        error = "没有可用的显示器，或显示器数量超过限制";
        return result;
    }
    qreal density = 1;
    for (const auto &screen : screens) {
        const auto box = screen.desktop;
        if (screen.image.isNull() || box.isEmpty() || !qIsFinite(box.x()) || !qIsFinite(box.y()) ||
            !qIsFinite(box.width()) || !qIsFinite(box.height()) || qAbs(box.x()) > 1000000 ||
            qAbs(box.y()) > 1000000 || box.width() > 1000000 || box.height() > 1000000) {
            error = "显示器图像或逻辑坐标无效";
            return {};
        }
        result.desktop = result.desktop.united(box);
        density = qMax(density, qMax(screen.image.width() / box.width(), screen.image.height() / box.height()));
    }
    const qreal width = std::ceil(result.desktop.width() * density);
    const qreal height = std::ceil(result.desktop.height() * density);
    if (!qIsFinite(width) || !qIsFinite(height) || width > 65535 || height > 65535 || width * height > 100000000) {
        error = "拼接桌面超过 1 亿像素限制，请选择单个显示器截图";
        return {};
    }
    result.screenName = screens.first().name;
    result.screens = std::move(screens);
    if (result.screens.size() == 1) {
        result.image = result.screens.first().image;
        result.image.setDevicePixelRatio(1);
        return result;
    }
    result.image = QImage(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
    if (result.image.isNull()) {
        error = "多屏截图内存不足，请选择单个显示器";
        return {};
    }
    result.image.fill(Qt::transparent);
    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &screen : result.screens) {
        const QRectF destination(result.toPixel(screen.desktop.topLeft()), result.toPixel(screen.desktop.bottomRight()));
        painter.drawImage(destination, screen.image);
    }
    painter.end();
    return result;
}
std::unique_ptr<ICaptureBackend> createCaptureBackend() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsCapture>();
#else
    if (QGuiApplication::platformName().contains("wayland") ||
        (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" &&
         QGuiApplication::platformName() != "offscreen" && !qEnvironmentVariableIsSet("SNAPLING_TEST_X11")))
        return std::make_unique<WaylandCapture>();
    return std::make_unique<X11Capture>();
#endif
}
QString platformDescription() {
    QString s = QString("%1 · Qt %2\n").arg(QGuiApplication::platformName(), qVersion());
    for (auto *screen : QGuiApplication::screens())
        s += QString("%1: %2×%3，DPR %4，DPI %5\n")
                 .arg(screen->name())
                 .arg(screen->size().width())
                 .arg(screen->size().height())
                 .arg(screen->devicePixelRatio())
                 .arg(screen->logicalDotsPerInch());
    return s;
}
} // namespace SnapLing

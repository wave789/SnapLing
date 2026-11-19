#include "CaptureService.h"
#include <QGuiApplication>
#include <QScreen>
#ifdef Q_OS_WIN
#include "platform/windows/WindowsCapture.h"
#else
#include "platform/linux/wayland/WaylandCapture.h"
#include "platform/linux/x11/X11Capture.h"
#endif
namespace SnapLing {
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

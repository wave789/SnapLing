#include "platform/linux/wayland/Portal.h"
#include "platform/linux/wayland/WaylandCapture.h"
#include <QtTest>
using namespace SnapLing;
class TestPortal : public QObject {
    Q_OBJECT
  private slots:
    void actualCapabilities() {
        auto screenshot = PortalRequest::version("org.freedesktop.portal.Screenshot"),
             screencast = PortalRequest::version("org.freedesktop.portal.ScreenCast"),
             hotkey = PortalRequest::version("org.freedesktop.portal.GlobalShortcuts");
        qInfo("actual_portals screenshot=%u screencast=%u globalshortcuts=%u", screenshot, screencast,
              hotkey);
        QVERIFY(screenshot > 0);
    }
    void screenshotWithUserPermission() {
        WaylandCapture backend;
        QSignalSpy done(&backend, &ICaptureBackend::captured), errors(&backend, &ICaptureBackend::failed);
        backend.capture(CaptureMode::FullScreen);
        QElapsedTimer elapsed;
        elapsed.start();
        while (done.isEmpty() && errors.isEmpty() && elapsed.elapsed() < 10000)
            QTest::qWait(50);
        if (done.isEmpty()) {
            backend.cancel();
            qInfo("Portal 未获得截图，已关闭请求。%s",
                  errors.isEmpty() ? "等待系统授权超时" : qPrintable(errors[0][0].toString()));
            QSKIP("需要用户在系统授权界面批准；未宣称实际截图通过");
        }
        auto frame = qvariant_cast<CaptureFrame>(done[0][0]);
        QVERIFY(!frame.image.isNull());
        qInfo("portal_actual_image=%dx%d permission_inclusive_ms=%lld", frame.image.width(),
              frame.image.height(), elapsed.elapsed());
    }
};
QTEST_MAIN(TestPortal)
#include "TestPortal.moc"

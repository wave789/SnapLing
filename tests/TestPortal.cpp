#include "app/Application.h"
#include "platform/linux/wayland/Portal.h"
#include "platform/linux/wayland/WaylandCapture.h"
#include "selection/SelectionOverlay.h"
#include <QApplication>
#include <QDBusConnection>
#include <QtTest>
using namespace SnapLing;

// The private integration backend asks for actual client-side Qt window state
// at the instant the Portal frontend calls AccessDialog or Screenshot.
class WindowProbe : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.snapling.Test.WindowProbe")
  public slots:
    QString MainWindowState() const {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (qobject_cast<QMainWindow *>(widget) && widget->isVisible())
                return widget->isActiveWindow() ? "focused" : "visible";
        }
        return "hidden";
    }
};

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
        backend.setMethod(WaylandCapture::Method::Screenshot);
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

    void firstCaptureWithForegroundConsent() {
        if (qEnvironmentVariable("SNAPLING_IDENTITY_TEST_PRIVATE_BUS") != "1")
            QSKIP("Only run with the private frontend and synthetic screenshots");
        Q_INIT_RESOURCE(resources);
        QGuiApplication::setDesktopFileName("io.snapling.SnapLing");
        WindowProbe probe;
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("io.snapling.Test.WindowProbe"));
        QVERIFY(bus.registerObject("/probe", &probe, QDBusConnection::ExportAllSlots));
        auto backend = std::make_unique<WaylandCapture>();
        backend->setMethod(WaylandCapture::Method::Screenshot);
        QSignalSpy frames(backend.get(), &ICaptureBackend::captured);
        QSignalSpy errors(backend.get(), &ICaptureBackend::failed);
        Application application(nullptr, std::move(backend));
        application.show();
        QTRY_COMPARE(probe.MainWindowState(), QString("focused"));
        application.command({"--capture"});
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !errors.isEmpty(), 10000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.front().front().toString()));
        QCOMPARE(frames.count(), 1);
        const auto frame = qvariant_cast<CaptureFrame>(frames.front().front());
        QCOMPARE(frame.image.size(), QSize(240, 320));
        QCOMPARE(frame.image.pixelColor(0, 0), QColor(0x22, 0x88, 0x55));
        QCOMPARE(probe.MainWindowState(), QString("hidden"));
        int overlays = 0;
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (qobject_cast<SelectionOverlay *>(widget) && widget->isVisible())
                ++overlays;
        }
        QCOMPARE(overlays, 1);
        qInfo("portal_actual_image=240x320 main_hidden=1 selection_overlays=1");
    }
};
QTEST_MAIN(TestPortal)
#include "TestPortal.moc"

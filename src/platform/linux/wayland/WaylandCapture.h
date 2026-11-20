#pragma once
#include "capture/CaptureService.h"
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <vector>
class QDBusMessage;
namespace SnapLing {
class PortalRequest;
class PipeWireStream;
class WaylandCapture final : public ICaptureBackend {
    Q_OBJECT
  public:
    using StreamFactory = std::function<std::unique_ptr<PipeWireStream>()>;
    using ScreenProvider = std::function<QVector<CaptureScreen>()>;
    explicit WaylandCapture(QObject *parent = nullptr, StreamFactory factory = {}, ScreenProvider screens = {});
    ~WaylandCapture() override;
    void capture(CaptureMode, int screen = -1) override;
    void cancel() override;
    QString name() const override {
        return method == Method::Screenshot ? "Wayland · 单次桌面截图" : "Wayland · ScreenCast 兼容截图";
    }
    bool hasWindowPicking() const override {
        return false;
    }
    void startSharing();
    void stopSharing();
    void resetScreenSelection();
    void resetScreenshotPermission();
    bool sharing() const {
        return active;
    }
    QString capabilities() const;
    enum class Method { Screenshot, ScreenCast };
    void setMethod(Method method);
  signals:
    void sharingChanged(bool active);
    void screenshotPermissionReset();
  private slots:
    void sessionClosed(const QDBusMessage &message);

  private:
    void portalCapture();
    void reportScreenshotFailure(QString error);
    void beginSharing(bool temporary);
    void resetSharing();
    void failSharing(QString error);
    void waitForFrame();
    void deliverFrame();
    quint64 generation = 0;
    void selectSources();
    void startSession();
    void openRemote(const QVariantMap &data);
    QVector<CaptureScreen> currentScreens() const;
    bool requestedSourcesAvailable();
    QPointer<PortalRequest> request;
    QString session;
    bool active = false;
    bool starting = false, temporarySharing = false, capturePending = false;
    QTimer frameTimer;
    QElapsedTimer frameWait;
    bool composing = false, allScreens = true;
    bool resettingPermission = false;
    Method method = Method::Screenshot;
    uint screenCastVersion = 0;
    QString requestedScreen;
    QVector<CaptureScreen> layout;
    StreamFactory streamFactory;
    ScreenProvider screenProvider;
#ifdef SNAPLING_HAVE_PIPEWIRE
    struct SharedSource {
        uint node = 0;
        QString serial;
        CaptureScreen screen;
        std::unique_ptr<PipeWireStream> stream;
    };
    std::vector<SharedSource> sources;
    int opening = 0;
#endif
};
} // namespace SnapLing

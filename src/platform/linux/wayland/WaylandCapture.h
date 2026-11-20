#pragma once
#include "capture/CaptureService.h"
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
namespace SnapLing {
class PortalRequest;
class PipeWireStream;
class WaylandCapture final : public ICaptureBackend {
    Q_OBJECT
  public:
    using StreamFactory = std::function<std::unique_ptr<PipeWireStream>()>;
    explicit WaylandCapture(QObject *parent = nullptr, StreamFactory factory = {});
    ~WaylandCapture() override;
    void capture(CaptureMode, int screen = -1) override;
    void cancel() override;
    QString name() const override {
#ifdef SNAPLING_HAVE_PIPEWIRE
        return "Wayland ScreenCast / PipeWire 即时截图";
#else
        return "Wayland Screenshot Portal（PipeWire 未启用）";
#endif
    }
    bool hasWindowPicking() const override {
        return false;
    }
    void startSharing();
    void stopSharing();
    bool sharing() const {
        return active;
    }
    QString capabilities() const;
  signals:
    void sharingChanged(bool active);
  private slots:
    void sessionClosed();

  private:
    void portalCapture();
    void beginSharing(bool temporary);
    void resetSharing();
    void failSharing(QString error);
    void waitForFrame();
    void deliverFrame();
    quint64 generation = 0;
    void selectSources();
    void startSession();
    void openRemote(const QVariantMap &data);
    QPointer<PortalRequest> request;
    QString session;
    bool active = false;
    bool starting = false, temporarySharing = false, capturePending = false;
    QTimer frameTimer;
    QElapsedTimer frameWait;
    QRectF sharedDesktop;
    StreamFactory streamFactory;
#ifdef SNAPLING_HAVE_PIPEWIRE
    std::unique_ptr<PipeWireStream> stream;
#endif
};
} // namespace SnapLing

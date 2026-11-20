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
    void resetScreenSelection();
    bool sharing() const {
        return active;
    }
    QString capabilities() const;
  signals:
    void sharingChanged(bool active);
  private slots:
    void sessionClosed(const QDBusMessage &message);

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
    QVector<CaptureScreen> currentScreens() const;
    bool requestedSourcesAvailable();
    QPointer<PortalRequest> request;
    QString session;
    bool active = false;
    bool starting = false, temporarySharing = false, capturePending = false;
    QTimer frameTimer;
    QElapsedTimer frameWait;
    bool composing = false, allScreens = true;
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

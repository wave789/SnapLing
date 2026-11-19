#pragma once
#include "capture/CaptureService.h"
#include <QPointer>
namespace SnapLing {
class PortalRequest;
class PipeWireStream;
class WaylandCapture final : public ICaptureBackend {
    Q_OBJECT
  public:
    explicit WaylandCapture(QObject *parent = nullptr);
    ~WaylandCapture() override;
    void capture(CaptureMode, int screen = -1) override;
    void cancel() override;
    QString name() const override {
        return "Wayland Screenshot Portal";
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
    void portalCapture(CaptureMode mode, bool interactive);
    quint64 generation = 0;
    void selectSources();
    void startSession();
    void openRemote(const QVariantMap &data);
    QPointer<PortalRequest> request;
    QString session;
    bool active = false;
#ifdef SNAPLING_HAVE_PIPEWIRE
    std::unique_ptr<PipeWireStream> stream;
#endif
};
} // namespace SnapLing

#pragma once
#include "capture/CaptureService.h"
namespace SnapLing {
class X11Capture final : public ICaptureBackend {
    Q_OBJECT
  public:
    using ICaptureBackend::ICaptureBackend;
    void capture(CaptureMode mode, int screen = -1) override;
    void cancel() override {
        if (token)
            token->cancel();
    }
    QString name() const override {
        return "X11 原生桌面捕获";
    }
    bool hasWindowPicking() const override {
        return true;
    }

  private:
    Cancellation token;
};
} // namespace SnapLing

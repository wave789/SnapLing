#pragma once
#include "capture/CaptureService.h"
#include "hotkey/Hotkey.h"
#include "settings/Settings.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QProgressDialog>
#include <QSystemTrayIcon>
namespace SnapLing {
class EditorWindow;
class EditorSession;
class SelectionOverlay;
class Application final : public QObject {
    Q_OBJECT
  public:
    explicit Application(QObject *parent = nullptr, std::unique_ptr<ICaptureBackend> captureBackend = {});
    ~Application() override;
    void command(QStringList args);
    void show();

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void capture(CaptureMode mode = CaptureMode::Region, bool ocr = false, bool translate = false,
                 int delay = 0, bool explicitConsent = false);
    void beginCapture();
    void prepareDesktopCapture(bool mainWasVisible = false);
    void scheduleCapture(int milliseconds);
    void showConsentWindow();
    void requestConsentWhenFocused();
    void presentWindow(QWidget &window);
    void activatePresentedWindow();
    bool finishConsent();
    void reportCaptureFailure(const QString &message);
    void restoreWindows();
    void showCaptureOverlays(CaptureFrame frame);
    void closeCaptureOverlays(SelectionOverlay *except = nullptr);
    QList<QPointer<SelectionOverlay>> captureOverlays;
    QList<QPointer<QWidget>> hiddenWindows;
    bool preparing = false;
    enum class Consent { None, Checking, WaitingForFocus, Requesting };
    Consent consent = Consent::None;
    std::unique_ptr<QDialog> consentWindow;
    quint64 captureGeneration = 0;
    void edit(QImage image, bool ocr = false, bool translate = false);
    void edit(std::shared_ptr<EditorSession> session);
    void open(QString path = {});
    void settingsDialog();
    void historyDialog();
    void applyTheme();
    void cancelCapture();
    void bindHotkeys();
    void configureGnomeCaptureAcceleration();
    Settings settings;
    ProviderConfig config;
    std::unique_ptr<ICaptureBackend> backend;
    std::unique_ptr<IHotkeyBackend> hotkey;
    std::vector<std::unique_ptr<IHotkeyBackend>> extraHotkeys;
    QMainWindow main;
    QComboBox *modeBox, *screenBox;
    QLabel *status, *hotkeyStatus;
    QString captureActivationToken, windowActivationToken;
    QPointer<QWidget> activationTarget;
    QPointer<QProgressDialog> countdown;
    QSystemTrayIcon tray;
    CaptureMode pendingMode = CaptureMode::Region;
    bool pendingOcr = false, pendingTranslation = false, capturing = false;
    int remaining = 0, pendingDelay = 0, pendingScreen = -1;
    int visibleWindowSettleMs = 300;
    QElapsedTimer captureTimer;
};
} // namespace SnapLing

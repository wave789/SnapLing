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
class Application final : public QObject {
    Q_OBJECT
  public:
    explicit Application(QObject *parent = nullptr);
    ~Application() override;
    void command(QStringList args);
    void show();

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void capture(CaptureMode mode = CaptureMode::Region, bool ocr = false, bool translate = false,
                 int delay = 0);
    void beginCapture();
    void restoreWindows();
    QList<QPointer<QWidget>> hiddenWindows;
    bool preparing = false;
    void edit(QImage image, bool ocr = false, bool translate = false);
    void edit(std::shared_ptr<EditorSession> session);
    void open(QString path = {});
    void settingsDialog();
    void historyDialog();
    void applyTheme();
    void sharing();
    void bindHotkeys();
    Settings settings;
    ProviderConfig config;
    std::unique_ptr<ICaptureBackend> backend;
    std::unique_ptr<IHotkeyBackend> hotkey;
    std::vector<std::unique_ptr<IHotkeyBackend>> extraHotkeys;
    QMainWindow main;
    QComboBox *modeBox, *screenBox;
    QLabel *status, *sharingLabel;
    QPointer<QProgressDialog> countdown;
    QSystemTrayIcon tray;
    CaptureMode pendingMode = CaptureMode::Region;
    bool pendingOcr = false, pendingTranslation = false, capturing = false;
    int remaining = 0;
    QElapsedTimer captureTimer;
};
} // namespace SnapLing

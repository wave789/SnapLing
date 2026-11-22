#include "Application.h"
#include "history/HistoryStore.h"
#include "selection/SelectionOverlay.h"
#include "ui/EditorWindow.h"
#include "ui/SettingsDialog.h"
#include <QInputDialog>
#include <QJsonObject>
#ifndef Q_OS_WIN
#include "platform/linux/wayland/WaylandCapture.h"
#endif
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSplitter>
#include <QStyleFactory>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
namespace SnapLing {
#ifndef Q_OS_WIN
static bool isGnomeSession() {
    const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts);
    if (!desktops.isEmpty())
        return desktops.contains("GNOME", Qt::CaseInsensitive);
    // Some terminal launchers preserve only the login session name.
    auto session = qEnvironmentVariable("XDG_SESSION_DESKTOP");
    if (session.isEmpty())
        session = qEnvironmentVariable("DESKTOP_SESSION");
    session = QFileInfo(session).completeBaseName().toLower();
    const QStringList gnomeSessions{"gnome", "gnome-wayland", "gnome-xorg", "gnome-classic",
                                    "ubuntu", "ubuntu-wayland", "ubuntu-xorg"};
    return gnomeSessions.contains(session);
}
#endif
Application::Application(QObject *p, std::unique_ptr<ICaptureBackend> captureBackend)
    : QObject(p), config(settings.provider()),
      backend(captureBackend ? std::move(captureBackend) : createCaptureBackend()),
      hotkey(createHotkeyBackend()) {
    main.setWindowTitle("SnapLing · 智能截图");
    main.setObjectName("snaplingMainWindow");
    main.setWindowIcon(QIcon(":/snapling.svg"));
    main.resize(680, 500);
    main.installEventFilter(this);
    auto *body = new QWidget;
    main.setCentralWidget(body);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(30, 24, 30, 24);
    layout->setSpacing(16);
    auto *title = new QLabel("SnapLing");
    QFont font = title->font();
    font.setPointSize(26);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto *subtitle = new QLabel("截图 · 标注 · 离线文字识别 · 原地翻译");
    layout->addWidget(subtitle);
    auto *row = new QHBoxLayout;
    modeBox = new QComboBox;
    modeBox->addItems({"矩形区域", "全屏", "窗口", "椭圆", "多边形", "自由曲线"});
    row->addWidget(modeBox);
    screenBox = new QComboBox;
    screenBox->setObjectName("captureScreenSelector");
    auto refreshScreens = [this] {
        auto selected = screenBox->currentText();
        const auto name = screenBox->currentData(Qt::UserRole + 1).toString();
        screenBox->clear();
        screenBox->addItem("整个桌面（全部屏幕）", -1);
        screenBox->addItem("主屏幕", -2);
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const auto *screen = screens[i];
            screenBox->addItem(QString("%1 · %2×%3 · %4%")
                                  .arg(screen->name()).arg(screen->size().width()).arg(screen->size().height())
                                  .arg(qRound(screen->devicePixelRatio() * 100)), i);
            screenBox->setItemData(screenBox->count() - 1, screen->name(), Qt::UserRole + 1);
        }
        int index = name.isEmpty() ? screenBox->findText(selected) : screenBox->findData(name, Qt::UserRole + 1);
        if (index >= 0)
            screenBox->setCurrentIndex(index);
    };
    refreshScreens();
    connect(qApp, &QGuiApplication::screenAdded, this, [refreshScreens](QScreen *) { refreshScreens(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [refreshScreens](QScreen *) { refreshScreens(); });
    row->addWidget(screenBox);
    auto *captureButton = new QPushButton("开始截图");
    captureButton->setObjectName("captureButton");
    captureButton->setDefault(true);
    captureButton->setMinimumHeight(42);
    row->addWidget(captureButton);
    layout->addLayout(row);
    connect(captureButton, &QPushButton::clicked, this,
            [this] { capture(CaptureMode(modeBox->currentIndex())); });
    auto *operations = new QHBoxLayout;
    auto button = [&](QString name, auto fn) {
        auto *b = new QPushButton(name);
        operations->addWidget(b);
        connect(b, &QPushButton::clicked, this, fn);
    };
    button("延时截图…", [this] {
        bool ok;
        int sec = QInputDialog::getInt(&main, "延时截图", "延时秒数（Esc 可取消）", 3, 0, 3600, 1, &ok);
        if (ok)
            capture(CaptureMode(modeBox->currentIndex()), false, false, sec);
    });
    button("打开图片/工程", [this] { open(); });
    button("从剪贴板导入", [this] {
        auto image = QApplication::clipboard()->image();
        if (image.isNull())
            QMessageBox::information(&main, "剪贴板", "剪贴板没有图片");
        else
            edit(image);
    });
    layout->addLayout(operations);
    auto *second = new QHBoxLayout;
    auto add = [&](QString name, auto fn) {
        auto *b = new QPushButton(name);
        second->addWidget(b);
        connect(b, &QPushButton::clicked, this, fn);
    };
    add("OCR 截图", [this] { capture(CaptureMode::Region, true); });
    add("翻译截图", [this] { capture(CaptureMode::Region, false, true); });
    add("历史", [this] { historyDialog(); });
    add("设置", [this] { settingsDialog(); });
    layout->addLayout(second);
    status = new QLabel;
    status->setObjectName("captureStatus");
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->insertWidget(2, status);
    hotkeyStatus = new QLabel;
    hotkeyStatus->setWordWrap(true);
    hotkeyStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hotkeyStatus->setObjectName("hotkeyStatus");
    layout->addWidget(hotkeyStatus);
    auto *hotkeyButtons = new QHBoxLayout;
    auto *retryHotkey = new QPushButton("重新注册快捷键", body);
    hotkeyButtons->addWidget(retryHotkey);
    connect(retryHotkey, &QPushButton::clicked, this, &Application::bindHotkeys);
    auto *copyCommand = new QPushButton("复制系统快捷键命令", body);
    hotkeyButtons->addWidget(copyCommand);
    const bool waylandSession = QGuiApplication::platformName().contains("wayland") ||
                                qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
    copyCommand->setVisible(waylandSession);
    retryHotkey->setVisible(waylandSession);
    connect(copyCommand, &QPushButton::clicked, this, [this] {
        QString path = QCoreApplication::applicationFilePath();
        path.replace('"', "\\\"");
        QApplication::clipboard()->setText("\"" + path + "\" --capture");
        hotkeyStatus->setText("命令已复制。在 Ubuntu 设置 → 键盘 → 自定义快捷键中粘贴并绑定 Ctrl+Alt+A。");
    });
    layout->addLayout(hotkeyButtons);
    auto *capabilities = new QLabel(platformDescription() + backend->name());
    capabilities->setWordWrap(true);
    capabilities->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(capabilities);
#ifndef Q_OS_WIN
    if (auto *wayland = qobject_cast<WaylandCapture *>(backend.get())) {
        connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
            if (consent == Consent::WaitingForFocus)
                QTimer::singleShot(0, this, &Application::requestConsentWhenFocused);
        });
        connect(wayland, &WaylandCapture::screenshotAuthorized, this, [this] {
            if (preparing && consent == Consent::Requesting)
                prepareDesktopCapture();
        });
        connect(wayland, &WaylandCapture::compositorSelectionCancelled, this, [this] {
            cancelCapture();
            status->setText("截图已取消。");
        });
        capabilities->setText(capabilities->text() + "\n" + wayland->capabilities());
        auto *regrant = new QPushButton("重新请求截图授权");
        regrant->setObjectName("retryScreenshotPermission");
        regrant->setToolTip("清除 SnapLing 自己的截图授权记录，再由系统询问是否允许；不启用屏幕共享。");
        layout->insertWidget(3, regrant);
        connect(regrant, &QPushButton::clicked, this, [this, wayland] {
            if (capturing || preparing || countdown) {
                status->setText("请先完成或取消当前截图，再重新请求截图授权。");
                return;
            }
            status->setStyleSheet({});
            status->setText("正在重新请求 SnapLing 的系统截图授权…");
            preparing = true;
            wayland->resetScreenshotPermission();
        });
        connect(wayland, &WaylandCapture::screenshotPermissionReset, this, [this, wayland] {
            preparing = false;
            wayland->setMethod(WaylandCapture::Method::Screenshot);
            settings.set("capture/waylandMethod", "screenshot");
            if (auto *button = main.findChild<QPushButton *>("resetCompatibilityCapture"))
                button->hide();
            // The user explicitly requested consent in this window, even on an unidentified desktop.
            capture(CaptureMode(modeBox->currentIndex()), false, false, 0, true);
        });
        auto *reselect = new QPushButton("重置兼容截图授权", body);
        reselect->setObjectName("resetCompatibilityCapture");
        reselect->setVisible(settings.value("capture/waylandMethod", "screenshot").toString() == "screencast");
        layout->addWidget(reselect);
        connect(reselect, &QPushButton::clicked, this, [this, wayland] {
            if (capturing || preparing || countdown) {
                status->setText("请先完成或取消当前截图。");
                return;
            }
            wayland->resetScreenSelection();
            status->setText("兼容模式：请在系统授权窗口选择需要截图的显示器。");
            capture(CaptureMode(modeBox->currentIndex()));
        });
    }
#endif
    layout->addStretch();
    auto *quit = new QPushButton("退出 SnapLing");
    layout->addWidget(quit);
    connect(quit, &QPushButton::clicked, qApp, &QCoreApplication::quit);
    connect(backend.get(), &ICaptureBackend::captured, this, [this](CaptureFrame frame) {
        if (!capturing)
            return;
        if (frame.image.isNull()) {
            capturing = false;
            restoreWindows();
            status->setText("截图后端返回空图像");
            show();
            return;
        }
        qInfo("capture_ms=%lld trigger_to_pixels_ms=%lld screens=%lld", frame.captureMs,
              captureTimer.elapsed(), qlonglong(frame.screens.size()));
        if (pendingMode == CaptureMode::Window && !backend->hasWindowPicking())
            status->setText("在 SnapLing 选区中框选窗口内容，松开鼠标后即可标注。");
        showCaptureOverlays(std::move(frame));
    });
    connect(backend.get(), &ICaptureBackend::failed, this, [this](QString e) {
        qWarning("capture_failed stage=%s reason=%s", captureOverlays.isEmpty() ? "capture" : "editing", qPrintable(e));
        status->setText(e);
        status->setStyleSheet("QLabel{color:#d62f36;border:1px solid #d62f36;padding:10px;}");
        // 已冻结的截图与共享流独立；共享中断不能丢弃正在编辑的文档。
        if (!captureOverlays.isEmpty())
            return;
        capturing = false;
        closeCaptureOverlays();
        preparing = false;
        finishConsent();
        restoreWindows();
        show();
        if (tray.isVisible())
            tray.showMessage("截图失败", e, QSystemTrayIcon::Warning, 8000);
    });
    connect(hotkey.get(), &IHotkeyBackend::activated, this, [this] { capture(); });
    connect(hotkey.get(), &IHotkeyBackend::activationTokenAvailable, this,
            [this](QString token) { captureActivationToken = std::move(token); });
    connect(hotkey.get(), &IHotkeyBackend::status, this,
            [this](QString message, bool ok) {
                hotkeyStatus->setText(message);
                qInfo("hotkey_ready=%d status=%s", ok, qPrintable(message));
            });
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray.setIcon(QIcon(":/snapling.svg"));
        tray.setToolTip("SnapLing");
        auto *menu = new QMenu(&main);
        menu->addAction("截图", this, [this] { capture(); });
        menu->addAction("取消截图", this, &Application::cancelCapture);
        menu->addAction("打开 SnapLing", this, [this] { show(); });
        menu->addAction("设置", this, [this] { settingsDialog(); });
        menu->addAction("退出", qApp, &QCoreApplication::quit);
        tray.setContextMenu(menu);
        connect(&tray, &QSystemTrayIcon::activated, this, [this](auto reason) {
            if (reason == QSystemTrayIcon::Trigger)
                show();
        });
        tray.show();
    }
    applyTheme();
    QTimer::singleShot(0, this, [this] { bindHotkeys(); });
}
Application::~Application() {
    backend->cancel();
    closeCaptureOverlays();
    hotkey->unbind();
    QList<QWidget *> editors;
    for (auto *w : QApplication::topLevelWidgets())
        if (qobject_cast<EditorWindow *>(w) || qobject_cast<SelectionOverlay *>(w))
            editors << w;
    qDeleteAll(editors);
    QThreadPool::globalInstance()->waitForDone();
}
void Application::bindHotkeys() {
    hotkey->bind(QKeySequence(settings.value("hotkey/capture", "Ctrl+Alt+A").toString()));
    extraHotkeys.clear();
    const QStringList names{"full", "window", "delay", "ocr", "translate"};
    for (int i = 0; i < names.size(); ++i) {
        auto key = settings.value("hotkey/" + names[i]).toString();
        if (key.isEmpty())
            continue;
        auto h = createHotkeyBackend();
        connect(h.get(), &IHotkeyBackend::status, this, [this](QString m, bool) { hotkeyStatus->setText(m); });
        connect(h.get(), &IHotkeyBackend::activationTokenAvailable, this,
                [this](QString token) { captureActivationToken = std::move(token); });
        connect(h.get(), &IHotkeyBackend::activated, this, [this, i] {
            capture(i == 0   ? CaptureMode::FullScreen
                    : i == 1 ? CaptureMode::Window
                             : CaptureMode::Region,
                    i == 3, i == 4, i == 2 ? 3 : 0);
        });
        h->bind(QKeySequence(key));
        extraHotkeys.push_back(std::move(h));
    }
}
void Application::show() {
    const bool wayland = QGuiApplication::platformName().startsWith("wayland");
    if (wayland) {
        // Qt can request a startup token both on show() and activateWindow(). An unconsumed
        // token leaves GNOME's busy cursor active until its startup notification times out.
        main.setAttribute(Qt::WA_ShowWithoutActivating);
        mainActivationToken = captureActivationToken;
    }
    captureActivationToken.clear();
    if (main.isMinimized())
        main.setWindowState(main.windowState() & ~Qt::WindowMinimized);
    main.show();
    if (wayland) {
        if (auto *window = main.windowHandle())
            window->installEventFilter(this);
        activateMainWindow();
    } else {
        main.raise();
        main.activateWindow();
    }
}
void Application::activateMainWindow() {
    auto *window = main.windowHandle();
    if (mainActivationToken.isEmpty() || !main.isVisible() || !window || !window->isExposed())
        return;
    const auto previous = qgetenv("XDG_ACTIVATION_TOKEN");
    qputenv("XDG_ACTIVATION_TOKEN", mainActivationToken.toUtf8());
    mainActivationToken.clear();
    window->requestActivate();
    if (previous.isNull())
        qunsetenv("XDG_ACTIVATION_TOKEN");
    else
        qputenv("XDG_ACTIVATION_TOKEN", previous);
}
bool Application::eventFilter(QObject *o, QEvent *e) {
    if (o == main.windowHandle() && e->type() == QEvent::Expose && !mainActivationToken.isEmpty())
        activateMainWindow();
    if (o == &main && e->type() == QEvent::Hide)
        mainActivationToken.clear();
    if (consent == Consent::WaitingForFocus &&
        ((o == &main && e->type() == QEvent::WindowActivate) ||
         (o == main.windowHandle() && e->type() == QEvent::Expose)))
        QTimer::singleShot(0, this, &Application::requestConsentWhenFocused);
    if (o == &main && e->type() == QEvent::Close && consent != Consent::None)
        cancelCapture();
    if (o == &main && e->type() == QEvent::Close && settings.value("general/background", true).toBool()) {
        main.hide();
        static_cast<QCloseEvent *>(e)->ignore();
        return true;
    }
    if (o == &main && e->type() == QEvent::Close && !settings.value("general/background", true).toBool())
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    return QObject::eventFilter(o, e);
}
void Application::capture(CaptureMode mode, bool ocr, bool translate, int delay, bool explicitConsent) {
    if (capturing || countdown || preparing) {
        if (consent == Consent::WaitingForFocus) {
            show();
            requestConsentWhenFocused();
            return;
        }
        if (!captureOverlays.isEmpty()) {
            for (qsizetype i = 0; i < captureOverlays.size(); ++i) {
                auto overlay = captureOverlays[i];
                if (overlay) {
                    const bool activate = i == captureOverlays.size() - 1;
                    overlay->present(activate ? captureActivationToken : QString(), activate);
                }
            }
            captureActivationToken.clear();
        } else
            hotkeyStatus->setText("正在获取桌面，请完成系统授权。可从托盘取消截图后重试。");
        return;
    }
    int editors = 0;
    for (auto *w : QApplication::topLevelWidgets())
        if (qobject_cast<EditorWindow *>(w))
            ++editors;
    if (editors >= 8) {
        restoreWindows();
        status->setText("已有 8 个编辑窗口，请关闭暂不需要的窗口后继续截图。");
        show();
        return;
    }
    preparing = true;
    ++captureGeneration;
    status->setStyleSheet({});
    status->setText("正在获取桌面截图… 如系统显示截图授权窗口，请完成授权。");
    captureTimer.start();
    pendingMode = mode;
    pendingOcr = ocr;
    pendingTranslation = translate;
    pendingDelay = delay;
    pendingScreen = screenBox->currentData().toInt();
#ifndef Q_OS_WIN
    auto *wayland = qobject_cast<WaylandCapture *>(backend.get());
    if (wayland && wayland->usesScreenshot(pendingMode, pendingScreen) && (explicitConsent || isGnomeSession())) {
        consent = Consent::Checking;
        wayland->checkScreenshotPermission([this, job = captureGeneration](bool needsConsent) {
            if (!preparing || job != captureGeneration || consent != Consent::Checking)
                return;
            if (!needsConsent) {
                prepareDesktopCapture();
                return;
            }
            // GNOME Shell verifies the focused application's desktop ID before it opens AccessDialog.
            mainShownForConsent = !main.isVisible();
            mainWasMinimized = main.isMinimized();
            consent = Consent::WaitingForFocus;
            status->setText("首次截图需要系统授权。请点击此窗口，然后在系统询问时允许 SnapLing 截图。");
            show();
            if (auto *window = main.windowHandle())
                window->installEventFilter(this);
            QTimer::singleShot(0, this, &Application::requestConsentWhenFocused);
        });
        return;
    }
#else
    Q_UNUSED(explicitConsent)
#endif
    prepareDesktopCapture();
}
void Application::requestConsentWhenFocused() {
#ifndef Q_OS_WIN
    auto *window = main.windowHandle();
    if (consent != Consent::WaitingForFocus || !window || !window->isExposed() ||
        !main.isActiveWindow() || QGuiApplication::focusWindow() != window)
        return;
    consent = Consent::Requesting;
    status->setText("请在系统窗口中允许 SnapLing 截图。授权完成后将自动截取桌面。");
    qInfo("screenshot_consent=focused");
    static_cast<WaylandCapture *>(backend.get())->authorizeScreenshot();
#endif
}
bool Application::finishConsent() {
    const bool usedMain = consent == Consent::WaitingForFocus || consent == Consent::Requesting;
    consent = Consent::None;
    if (usedMain) {
        if (mainShownForConsent)
            main.hide();
        if (mainWasMinimized)
            main.setWindowState(main.windowState() | Qt::WindowMinimized);
    }
    return usedMain;
}
void Application::prepareDesktopCapture() {
    bool ownVisible = finishConsent();
    status->setText("正在获取桌面截图…");
    for (auto *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowType() != Qt::Popup) {
            ownVisible = true;
            hiddenWindows << w;
            w->hide();
        }
    const int delay = pendingDelay;
    if (delay > 0) {
        remaining = delay;
        countdown = new QProgressDialog("延时截图倒计时：" + QString::number(remaining) + " 秒", "取消", 0,
                                        delay, &main);
        countdown->setWindowModality(Qt::NonModal);
        countdown->setMinimumDuration(0);
        countdown->setValue(0);
        auto *timer = new QTimer(countdown);
        timer->setInterval(1000);
        connect(countdown, &QProgressDialog::canceled, this, [this] {
            if (countdown)
                countdown->deleteLater();
            countdown = nullptr;
            preparing = false;
            restoreWindows();
        });
        connect(timer, &QTimer::timeout, this, [this, timer, delay] {
            if (!countdown) {
                timer->stop();
                return;
            }
            --remaining;
            if (remaining <= 0) {
                auto d = countdown;
                countdown = nullptr;
                captureTimer.start();
                d->hide();
                d->deleteLater();
                scheduleCapture(160);
            } else {
                countdown->setLabelText(QString("延时截图：%1 秒，可取消").arg(remaining));
                countdown->setValue(delay - remaining);
            }
        });
        countdown->show();
        timer->start();
        return;
    }
    if (ownVisible)
        scheduleCapture(120);
    else
        beginCapture();
}
void Application::scheduleCapture(int milliseconds) {
    QTimer::singleShot(milliseconds, this, [this, job = captureGeneration] {
        if (job == captureGeneration)
            beginCapture();
    });
}
void Application::restoreWindows() {
    auto windows = hiddenWindows;
    hiddenWindows.clear();
    for (auto w : windows)
        if (w)
            w->show();
}
void Application::closeCaptureOverlays(SelectionOverlay *except) {
    const auto overlays = captureOverlays;
    captureOverlays.clear();
    for (auto overlay : overlays)
        if (overlay) {
            if (overlay == except)
                captureOverlays << overlay;
            else {
                disconnect(overlay, nullptr, this, nullptr);
                overlay->close();
            }
        }
}
void Application::showCaptureOverlays(CaptureFrame frame) {
    QVector<CaptureFrame> views;
    if (frame.screens.size() > 1 && pendingMode != CaptureMode::FullScreen) {
        for (const auto &screen : frame.screens) {
            CaptureFrame view;
            view.image = screen.image;
            view.desktop = screen.desktop;
            view.screenName = screen.name;
            views << std::move(view);
        }
    } else {
        if (pendingMode == CaptureMode::FullScreen && frame.screens.size() > 1)
            frame.selectionComplete = true;
        views << std::move(frame);
    }
    // Wayland 上每个输出单独创建全屏 surface，避免把整幅桌面缩到主屏。
    if (QGuiApplication::platformName().contains("wayland"))
        for (const auto &view : views) {
            bool found = false;
            for (auto *screen : QGuiApplication::screens())
                found |= screen->name() == view.screenName || screen->geometry().contains(view.desktop.center().toPoint());
            if (!found) {
                capturing = false;
                restoreWindows();
                status->setText("无法匹配截图所在显示器，请检查屏幕连接并重新截图。");
                show();
                return;
            }
        }
    // 全屏窗口各自保留目标输出，只有一个窗口请求键盘焦点；不能让各屏反复争抢激活。
    if (auto *pointerScreen = QGuiApplication::screenAt(QCursor::pos()))
        for (qsizetype i = 0; i < views.size(); ++i)
            if (views[i].screenName == pointerScreen->name()) {
                views.move(i, views.size() - 1);
                break;
            }
    for (qsizetype index = 0; index < views.size(); ++index) {
        auto &view = views[index];
        const bool compositorSelection = view.hasSelectionViewport();
        auto session = std::make_shared<EditorSession>(QImage(), settings, config);
        const bool activate = index == views.size() - 1;
        const auto token = activate ? captureActivationToken : QString();
        if (activate)
            captureActivationToken.clear();
        auto *overlay = new SelectionOverlay(std::move(view), pendingMode, nullptr, session, token, activate);
        captureOverlays << overlay;
        connect(overlay, &SelectionOverlay::advancedRequested, this, [this, overlay](auto session) {
            closeCaptureOverlays(overlay);
            captureOverlays.clear();
            capturing = false;
            restoreWindows();
            edit(std::move(session));
        });
        connect(overlay, &SelectionOverlay::quickEditingStarted, this, [this, overlay] {
            closeCaptureOverlays(overlay);
            if (pendingOcr || pendingTranslation)
                overlay->runOcr(pendingTranslation);
        });
        connect(overlay, &SelectionOverlay::cancelled, this, [this, overlay] {
            closeCaptureOverlays(overlay);
            captureOverlays.clear();
            capturing = false;
            restoreWindows();
        });
        connect(overlay, &SelectionOverlay::ready, this, [this, overlay, compositorSelection] {
            qInfo("%s=%lld screen=%s", compositorSelection ? "trigger_to_quick_edit_ms" : "trigger_to_selection_ms",
                  captureTimer.elapsed(), qPrintable(overlay->captureScreenName()));
            QTimer::singleShot(250, overlay, [overlay] {
                auto *window = overlay->windowHandle();
                qInfo("overlay_exposed=%d active=%d target=%s output=%s size=%dx%d",
                      window && window->isExposed(), window && window->isActive(),
                      qPrintable(overlay->captureScreenName()),
                      window && window->screen() ? qPrintable(window->screen()->name()) : "",
                      overlay->width(), overlay->height());
            });
        });
    }
}
void Application::beginCapture() {
    if (!preparing)
        return;
    preparing = false;
    if (capturing)
        return;
    capturing = true;
    int screen = pendingScreen;
    if (screen == -2) {
        auto screens = QGuiApplication::screens();
        screen = qMax(0, int(screens.indexOf(QGuiApplication::primaryScreen())));
    }
    backend->capture(pendingMode, screen);
}
void Application::edit(QImage image, bool ocr, bool translate) {
    auto session = std::make_shared<EditorSession>(image, settings, config);
    edit(session);
    if (ocr || translate)
        session->runOcr(translate);
}
void Application::edit(std::shared_ptr<EditorSession> session) {
    auto *w = new EditorWindow(session);
    connect(w, &EditorWindow::settingsRequested, this, &Application::settingsDialog);
    w->show();
    if (settings.value("general/autoCopy", false).toBool())
        QApplication::clipboard()->setImage(session->document.render());
}
void Application::open(QString path) {
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(&main, "打开图像或工程", {},
                                            "图像与工程 (*.png *.jpg *.jpeg *.bmp *.webp *.snapling)");
    if (path.isEmpty())
        return;
    if (path.endsWith(".snapling", Qt::CaseInsensitive)) {
        auto *w = new EditorWindow({}, settings, config);
        connect(w, &EditorWindow::settingsRequested, this, &Application::settingsDialog);
        if (w->loadProject(path))
            w->show();
        else
            w->deleteLater();
    } else {
        QImage image(path);
        if (image.isNull())
            QMessageBox::warning(&main, "打开失败", "图片无法解码，或文件不存在");
        else
            edit(image);
    }
}
void Application::settingsDialog() {
    SettingsDialog dialog(settings, config, &main);
    if (dialog.exec() == QDialog::Accepted) {
        applyTheme();
        bindHotkeys();
#ifndef Q_OS_WIN
        if (auto *wayland = qobject_cast<WaylandCapture *>(backend.get())) {
            const auto method = WaylandCapture::methodFromSetting(settings.value("capture/waylandMethod", "screenshot").toString());
            cancelCapture();
            wayland->setMethod(method);
            if (auto *button = main.findChild<QPushButton *>("resetCompatibilityCapture"))
                button->setVisible(method == WaylandCapture::Method::ScreenCast);
        }
#endif
    }
}
void Application::applyTheme() {
    int theme = settings.value("general/theme", 0).toInt();
    if (theme == 2)
        qApp->setStyleSheet(
            "QWidget{background:#202631;color:#e5e7eb} "
            "QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox{background:#303949;border:1px solid "
            "#526078;padding:5px} QPushButton,QToolButton{padding:6px;border:1px solid "
            "#526078;border-radius:4px} QPushButton:hover,QToolButton:hover{background:#3e4e67} "
            "QToolButton:checked{background:#1d4ed8}");
    else if (theme == 1)
        qApp->setStyleSheet("QWidget{background:#f6f8fc;color:#1c2535} QPushButton,QToolButton{padding:6px} "
                            "QLineEdit{background:white}");
    else
        qApp->setStyleSheet("");
}
void Application::cancelCapture() {
    ++captureGeneration;
    preparing = false;
    capturing = false;
    captureActivationToken.clear();
    if (countdown) {
        countdown->hide();
        countdown->deleteLater();
        countdown = nullptr;
    }
    backend->cancel();
    finishConsent();
    closeCaptureOverlays();
    restoreWindows();
}
void Application::historyDialog() {
    QDialog dialog(&main);
    dialog.setWindowTitle("截图历史");
    dialog.resize(760, 480);
    QVBoxLayout layout(&dialog);
    QLineEdit search;
    search.setPlaceholderText("按时间、路径、尺寸搜索");
    layout.addWidget(&search);
    QListWidget list;
    layout.addWidget(&list);
    HistoryStore h;
    auto refresh = [&] {
        list.clear();
        for (auto v : h.entries()) {
            auto o = v.toObject();
            QString text = QString("%1 · %2 × %3 · %4")
                               .arg(o["time"].toString())
                               .arg(o["width"].toInt())
                               .arg(o["height"].toInt())
                               .arg(o["path"].toString());
            if (!text.contains(search.text(), Qt::CaseInsensitive))
                continue;
            auto *item = new QListWidgetItem(text, &list);
            item->setData(Qt::UserRole, o);
        }
    };
    connect(&search, &QLineEdit::textChanged, &dialog, [&] { refresh(); });
    refresh();
    QHBoxLayout buttons;
    layout.addLayout(&buttons);
    QPushButton reopen("重新编辑"), copy("复制"), remove("删除"), clear("清空历史");
    for (auto *b : {&reopen, &copy, &remove, &clear})
        buttons.addWidget(b);
    connect(&reopen, &QPushButton::clicked, &dialog, [&] {
        if (list.currentItem())
            open(list.currentItem()->data(Qt::UserRole).toJsonObject()["path"].toString());
    });
    connect(&copy, &QPushButton::clicked, &dialog, [&] {
        if (list.currentItem())
            QApplication::clipboard()->setImage(
                QImage(list.currentItem()->data(Qt::UserRole).toJsonObject()["path"].toString()));
    });
    auto deleteFiles = [&] {
        auto answer =
            QMessageBox::question(&dialog, "删除历史", "是否连同图像文件一起删除？选择 No 仅删除记录。",
                                  QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        return answer;
    };
    connect(&remove, &QPushButton::clicked, &dialog, [&] {
        if (!list.currentItem())
            return;
        auto answer = deleteFiles();
        if (answer == QMessageBox::Cancel)
            return;
        QString error;
        h.remove(list.currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(),
                 answer == QMessageBox::Yes, error);
        refresh();
    });
    connect(&clear, &QPushButton::clicked, &dialog, [&] {
        auto answer = deleteFiles();
        if (answer == QMessageBox::Cancel)
            return;
        QString error;
        for (auto v : h.entries())
            h.remove(v.toObject()["id"].toString(), answer == QMessageBox::Yes, error);
        refresh();
    });
    dialog.exec();
}
void Application::command(QStringList args) {
    if (args.contains("--quit")) {
        qApp->quit();
        return;
    }
    auto value = [&](QString option) {
        int i = args.indexOf(option);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
    };
    captureActivationToken = value("--activation-token");
    if (args.contains("--cancel-capture")) {
        cancelCapture();
        show();
        return;
    }
    if (args.contains("--open")) {
        open(value("--open"));
        return;
    }
    if (args.contains("--clipboard")) {
        auto image = QApplication::clipboard()->image();
        if (!image.isNull())
            edit(image);
        else
            show();
        return;
    }
    if (args.contains("--capture") || args.contains("--full") || args.contains("--window") ||
        args.contains("--ocr") || args.contains("--translate") || args.contains("--delay")) {
        auto mode = args.contains("--full")     ? CaptureMode::FullScreen
                    : args.contains("--window") ? CaptureMode::Window
                                                : CaptureMode::Region;
        capture(mode, args.contains("--ocr"), args.contains("--translate"),
                qBound(0, value("--delay").toInt(), 3600));
        return;
    }
    if (!args.contains("--background"))
        show();
}
} // namespace SnapLing

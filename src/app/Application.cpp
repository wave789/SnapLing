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
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
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
namespace SnapLing {
Application::Application(QObject *p, std::unique_ptr<ICaptureBackend> captureBackend)
    : QObject(p), config(settings.provider()),
      backend(captureBackend ? std::move(captureBackend) : createCaptureBackend()),
      hotkey(createHotkeyBackend()) {
    main.setWindowTitle("SnapLing · 智能截图");
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
    auto refreshScreens = [this] {
        auto selected = screenBox->currentText();
        screenBox->clear();
        screenBox->addItem("主屏幕", -2);
        screenBox->addItem("整个桌面", -1);
        for (int i = 0; i < QGuiApplication::screens().size(); ++i)
            screenBox->addItem(QGuiApplication::screens()[i]->name(), i);
        int index = screenBox->findText(selected);
        if (index >= 0)
            screenBox->setCurrentIndex(index);
    };
    refreshScreens();
    connect(qApp, &QGuiApplication::screenAdded, this, [refreshScreens](QScreen *) { refreshScreens(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [refreshScreens](QScreen *) { refreshScreens(); });
    row->addWidget(screenBox);
    auto *captureButton = new QPushButton("开始截图");
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
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto *capabilities = new QLabel(platformDescription() + backend->name());
    capabilities->setWordWrap(true);
    capabilities->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(capabilities);
    sharingLabel = new QLabel;
    sharingLabel->setWordWrap(true);
    layout->addWidget(sharingLabel);
#ifndef Q_OS_WIN
    if (auto *wayland = qobject_cast<WaylandCapture *>(backend.get())) {
        capabilities->setText(capabilities->text() + "\n" + wayland->capabilities());
        auto *share = new QPushButton("开启 / 停止授权快速截图");
        layout->addWidget(share);
        connect(share, &QPushButton::clicked, this, &Application::sharing);
        connect(wayland, &WaylandCapture::sharingChanged, this, [this](bool active) {
            sharingLabel->setText(active ? "● 正在共享屏幕，仅缓存最新一帧。点击上方按钮可立即停止。"
                                         : "屏幕共享已停止");
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
        qInfo("capture_ms=%lld trigger_to_pixels_ms=%lld", frame.captureMs, captureTimer.elapsed());
        if (pendingMode == CaptureMode::Window && !backend->hasWindowPicking())
            status->setText("在 SnapLing 选区中框选窗口内容，松开鼠标后即可标注。");
        auto session = std::make_shared<EditorSession>(QImage(), settings, config);
        auto *overlay = new SelectionOverlay(std::move(frame), pendingMode, nullptr, session);
        connect(overlay, &SelectionOverlay::advancedRequested, this, [this](auto session) {
            capturing = false;
            restoreWindows();
            edit(std::move(session));
        });
        connect(overlay, &SelectionOverlay::quickEditingStarted, this, [this, overlay] {
            if (pendingOcr || pendingTranslation)
                overlay->runOcr(pendingTranslation);
        });
        connect(overlay, &SelectionOverlay::cancelled, this, [this] {
            capturing = false;
            restoreWindows();
        });
        connect(overlay, &SelectionOverlay::ready, this,
                [this] { qInfo("trigger_to_selection_ms=%lld", captureTimer.elapsed()); });
    });
    connect(backend.get(), &ICaptureBackend::failed, this, [this](QString e) {
        capturing = false;
        status->setText(e);
        preparing = false;
        restoreWindows();
        show();
    });
    connect(hotkey.get(), &IHotkeyBackend::activated, this, [this] { capture(); });
    connect(hotkey.get(), &IHotkeyBackend::status, this,
            [this](QString message, bool) { status->setText(message); });
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray.setIcon(QIcon(":/snapling.svg"));
        tray.setToolTip("SnapLing");
        auto *menu = new QMenu(&main);
        menu->addAction("截图", this, [this] { capture(); });
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
        connect(h.get(), &IHotkeyBackend::status, this, [this](QString m, bool) { status->setText(m); });
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
    main.show();
    main.raise();
    main.activateWindow();
}
bool Application::eventFilter(QObject *o, QEvent *e) {
    if (o == &main && e->type() == QEvent::Close && settings.value("general/background", true).toBool()) {
        main.hide();
        static_cast<QCloseEvent *>(e)->ignore();
        return true;
    }
    if (o == &main && e->type() == QEvent::Close && !settings.value("general/background", true).toBool())
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    return QObject::eventFilter(o, e);
}
void Application::capture(CaptureMode mode, bool ocr, bool translate, int delay) {
    if (capturing || countdown || preparing)
        return;
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
    captureTimer.start();
    pendingMode = mode;
    pendingOcr = ocr;
    pendingTranslation = translate;
    // 不显示或激活本程序任何窗口，先采集当前桌面；从本程序按钮触发时先隐藏自身窗口。
    bool ownVisible = false;
    for (auto *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowType() != Qt::Popup) {
            ownVisible = true;
            hiddenWindows << w;
            w->hide();
        }
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
                QTimer::singleShot(160, this, &Application::beginCapture);
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
        QTimer::singleShot(120, this, &Application::beginCapture);
    else
        beginCapture();
}
void Application::restoreWindows() {
    auto windows = hiddenWindows;
    hiddenWindows.clear();
    for (auto w : windows)
        if (w)
            w->show();
}
void Application::beginCapture() {
    preparing = false;
    if (capturing)
        return;
    capturing = true;
    int screen = screenBox->currentData().toInt();
    if (screen == -2) {
        auto screens = QGuiApplication::screens();
        screen = screens.indexOf(QGuiApplication::primaryScreen());
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
void Application::sharing() {
#ifndef Q_OS_WIN
    if (auto *b = qobject_cast<WaylandCapture *>(backend.get())) {
        if (b->sharing())
            b->stopSharing();
        else if (QMessageBox::question(&main, "授权快速截图",
                                       "开启后系统将持续共享所选屏幕。SnapLing "
                                       "仅缓存最新一帧，不录制、不保存屏幕流。可以随时停止。是否继续？",
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No) == QMessageBox::Yes) {
            sharingLabel->setText("正在等待系统屏幕共享授权…");
            b->startSharing();
        }
    }
#endif
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

#include "Application.h"
#include "SingleInstance.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif
int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    auto setDpi = reinterpret_cast<BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setDpi)
        setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else
        SetProcessDPIAware();
#else
    XInitThreads();
    // 本程序只使用光栅 Widgets，避免 XCB 加载不需要的 OpenGL 驱动。
    if (!qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION"))
        qputenv("QT_XCB_GL_INTEGRATION", "none");
#endif
    QElapsedTimer startup;
    startup.start();
    // Qt 初始化平台服务时就可能向 Portal 注册身份，不能等构造 QApplication 后才设置。
    QGuiApplication::setDesktopFileName("io.snapling.SnapLing");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
    app.setApplicationName("SnapLing");
    app.setOrganizationName("SnapLing");
    app.setApplicationVersion("0.5.4");
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(QIcon(":/snapling.svg"));
    QCommandLineParser parser;
    parser.setApplicationDescription("轻量截图、离线 OCR 与 AI 原地翻译");
    parser.addHelpOption();
    parser.addVersionOption();
    for (QString name : {"capture", "full", "window", "ocr", "translate", "clipboard", "background", "quit", "cancel-capture"})
        parser.addOption(QCommandLineOption(name, name));
    QCommandLineOption activationOption("activation-token", "Wayland activation token", "token");
    activationOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(activationOption);
    parser.addOption({"delay", "延时秒数", "seconds"});
    parser.addOption({"open", "打开图像或工程", "path"});
    parser.addOption({"exit-after", "自动退出，用于启动验证（毫秒）", "milliseconds"});
    parser.process(app);
    auto arguments = app.arguments().mid(1);
    const auto activationToken = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    if (!activationToken.isEmpty() && !arguments.contains("--activation-token"))
        arguments << "--activation-token" << activationToken;
    qunsetenv("XDG_ACTIVATION_TOKEN");
    SnapLing::SingleInstance instance;
    auto result = instance.start(arguments);
    if (result == SnapLing::SingleInstance::Result::Delivered)
        return 0;
    if (result == SnapLing::SingleInstance::Result::Error) {
        qCritical("已有实例不可达或单实例服务启动失败");
        return 2;
    }
    SnapLing::Application application;
    QObject::connect(&instance, &SnapLing::SingleInstance::command, &application,
                     &SnapLing::Application::command);
    QTimer::singleShot(0, &application, [&] { application.command(arguments); });
    if (parser.isSet("exit-after"))
        QTimer::singleShot(qMax(1, parser.value("exit-after").toInt()), &app, &QCoreApplication::quit);
    qInfo("startup_ms=%lld desktop_entry_installed=%d", startup.elapsed(),
          !QStandardPaths::locate(QStandardPaths::ApplicationsLocation, "io.snapling.SnapLing.desktop").isEmpty());
    return app.exec();
}

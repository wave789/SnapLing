#include "Autostart.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
namespace SnapLing {
bool Autostart::setEnabled(bool enabled, QString &error) {
#ifdef Q_OS_WIN
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled)
        run.setValue("SnapLing", "\"" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) +
                                     "\" --background");
    else
        run.remove("SnapLing");
    run.sync();
    if (run.status() != QSettings::NoError) {
        error = "无法更新当前用户的开机启动项";
        return false;
    }
    return true;
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
    QString path = dir + "/io.snapling.SnapLing.desktop";
    if (!enabled) {
        if (!QFile::exists(path) || QFile::remove(path))
            return true;
        error = "无法删除自动启动项";
        return false;
    }
    if (!QDir().mkpath(dir)) {
        error = "无法创建自动启动目录";
        return false;
    }
    auto executable = QCoreApplication::applicationFilePath()
                          .replace("\\", "\\\\")
                          .replace("\"", "\\\"")
                          .replace("`", "\\`")
                          .replace("$", "\\$");
    QByteArray bytes = QString("[Desktop Entry]\nType=Application\nName=SnapLing\nExec=\"%1\" "
                               "--background\nIcon=snapling\nX-GNOME-Autostart-enabled=true\n")
                           .arg(executable)
                           .toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = "写入自动启动项失败";
        return false;
    }
    return true;
#endif
}
} // namespace SnapLing

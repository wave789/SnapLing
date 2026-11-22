#include "SecretStore.h"
#include <QProcess>
#include <QStandardPaths>

namespace SnapLing::SecretStore {
namespace {
QString executable() {
    return QStandardPaths::findExecutable("secret-tool");
}

QStringList attributes(const QString &name) {
    return {"application", "snapling", "credential", name};
}

bool run(const QStringList &arguments, const QByteArray &input, QByteArray *output = nullptr,
         QString *error = nullptr) {
    const auto tool = executable();
    if (tool.isEmpty()) {
        if (error)
            *error = "找不到系统密钥环工具，请安装 libsecret-tools 后重新保存。";
        return false;
    }
    QProcess process;
    process.setProgram(tool);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(1000)) {
        if (error)
            *error = "无法启动系统密钥环工具，请检查 libsecret-tools 安装。";
        return false;
    }
    if (!input.isNull())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = "保存密钥超时，请解锁登录密钥环后重新保存。";
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = "系统密钥环未保存密钥，请确认 gnome-keyring 正在运行并已解锁，然后重新保存。";
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}
} // namespace

bool available() {
    return !executable().isEmpty();
}

QString baiduTranslationCredential(const QString &appId) {
    const auto id = appId.trimmed();
    return id.isEmpty() ? QStringLiteral("baidu-general-secret") : "baidu-general-secret:" + id;
}

QString loadBaiduTranslationSecret(const QString &appId) {
    auto secret = load(baiduTranslationCredential(appId));
    // Earlier versions stored one global Baidu credential. Saving again binds it to the AppID.
    if (secret.isEmpty() && !appId.trimmed().isEmpty())
        secret = load(baiduTranslationCredential());
    return secret;
}

QString load(const QString &name) {
    QStringList arguments{"lookup"};
    arguments << attributes(name);
    QByteArray output;
    if (!run(arguments, {}, &output))
        return {};
    return QString::fromUtf8(output).trimmed();
}

bool store(const QString &name, const QString &secret, QString *error) {
    if (error)
        error->clear();
    if (secret.isEmpty())
        return remove(name);
    QStringList arguments{"store", "--label=SnapLing translation credential"};
    arguments << attributes(name);
    return run(arguments, secret.toUtf8(), nullptr, error);
}

bool remove(const QString &name) {
    QStringList arguments{"clear"};
    arguments << attributes(name);
    return run(arguments, {});
}
} // namespace SnapLing::SecretStore

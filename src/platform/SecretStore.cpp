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

bool run(const QStringList &arguments, const QByteArray &input, QByteArray *output = nullptr) {
    const auto tool = executable();
    if (tool.isEmpty())
        return false;
    QProcess process;
    process.setProgram(tool);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(1000))
        return false;
    if (!input.isNull())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}
} // namespace

bool available() {
    return !executable().isEmpty();
}

QString baiduTranslationCredential() {
    return "baidu-general-secret";
}

QString load(const QString &name) {
    QStringList arguments{"lookup"};
    arguments << attributes(name);
    QByteArray output;
    if (!run(arguments, {}, &output))
        return {};
    return QString::fromUtf8(output).trimmed();
}

bool store(const QString &name, const QString &secret) {
    if (secret.isEmpty())
        return remove(name);
    QStringList arguments{"store", "--label=SnapLing translation credential"};
    arguments << attributes(name);
    return run(arguments, secret.toUtf8());
}

bool remove(const QString &name) {
    QStringList arguments{"clear"};
    arguments << attributes(name);
    return run(arguments, {});
}
} // namespace SnapLing::SecretStore

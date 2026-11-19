#include "Settings.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
namespace SnapLing {
bool ProviderConfig::isLocal() const {
    auto h = QUrl(baseUrl).host().toLower();
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}
Settings::Settings(QString file)
    : store(file.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini"
                : file,
            QSettings::IniFormat) {
    QDir().mkpath(QFileInfo(store.fileName()).absolutePath());
    if (store.value("schemaVersion", 0).toInt() < 1) {
        store.setValue("schemaVersion", 1);
        store.sync();
    }
}
QVariant Settings::value(QString key, QVariant fallback) const {
    return store.value(key, fallback);
}
void Settings::set(QString key, QVariant v) {
    store.setValue(key, v);
    store.sync();
}
ProviderConfig Settings::provider() const {
    ProviderConfig p;
    p.name = value("ai/name", p.name).toString();
    p.baseUrl = value("ai/url", p.baseUrl).toString();
    p.model = value("ai/model", p.model).toString();
    p.keyEnv = value("ai/keyEnv", p.keyEnv).toString();
    p.targetLanguage = value("ai/language", p.targetLanguage).toString();
    p.timeoutSeconds = value("ai/timeout", 90).toInt();
    p.maxTokens = value("ai/maxTokens", 4096).toInt();
    p.temperature = value("ai/temperature", 0.1).toDouble();
    p.structuredJson = value("ai/json", true).toBool();
    return p;
}
void Settings::saveProvider(const ProviderConfig &p) {
    set("ai/name", p.name);
    set("ai/url", p.baseUrl);
    set("ai/model", p.model);
    set("ai/keyEnv", p.keyEnv);
    set("ai/language", p.targetLanguage);
    set("ai/timeout", p.timeoutSeconds);
    set("ai/maxTokens", p.maxTokens);
    set("ai/temperature", p.temperature);
    set("ai/json", p.structuredJson);
}
} // namespace SnapLing

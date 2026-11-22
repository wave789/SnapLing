#include "Settings.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
namespace SnapLing {
QString translationBackendId(TranslationBackend backend) {
    return backend == TranslationBackend::BaiduGeneral ? "baidu" : "openai";
}
TranslationBackend translationBackendFromId(const QString &id) {
    return id == "baidu" ? TranslationBackend::BaiduGeneral : TranslationBackend::OpenAiCompatible;
}
bool ProviderConfig::isLocal() const {
    if (backend != TranslationBackend::OpenAiCompatible)
        return false;
    auto h = QUrl(baseUrl).host().toLower();
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}
Settings::Settings(QString file)
    : store(file.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini"
                : file,
            QSettings::IniFormat) {
    QDir().mkpath(QFileInfo(store.fileName()).absolutePath());
    const int schema = store.value("schemaVersion", 0).toInt();
    if (schema < 2) {
        const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts);
        const bool gnomeWayland =
            (desktops.contains("GNOME", Qt::CaseInsensitive) ||
             qEnvironmentVariable("XDG_SESSION_DESKTOP").startsWith("gnome", Qt::CaseInsensitive) ||
             qEnvironmentVariable("XDG_SESSION_DESKTOP").startsWith("ubuntu", Qt::CaseInsensitive)) &&
            qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
        if (gnomeWayland && store.value("capture/waylandMethod").toString() == "screenshot")
            store.setValue("capture/waylandMethod", "compositor");
        store.setValue("schemaVersion", 2);
        store.sync();
    }
}
QVariant Settings::value(QString key, QVariant fallback) const {
    return store.value(key, fallback);
}
bool Settings::contains(QString key) const {
    return store.contains(key);
}
void Settings::set(QString key, QVariant v) {
    store.setValue(key, v);
    store.sync();
}
ProviderConfig Settings::provider() const {
    ProviderConfig p;
    p.backend = translationBackendFromId(value("ai/backend", "openai").toString());
    p.name = value("ai/name", p.name).toString();
    p.baseUrl = value("ai/url", p.baseUrl).toString();
    p.model = value("ai/model", p.model).toString();
    p.keyEnv = value("ai/keyEnv", p.keyEnv).toString();
    p.appId = value("ai/appId").toString();
    p.targetLanguage = value("ai/language", p.targetLanguage).toString();
    p.timeoutSeconds = value("ai/timeout", 90).toInt();
    p.maxTokens = value("ai/maxTokens", 4096).toInt();
    p.temperature = value("ai/temperature", 0.1).toDouble();
    p.structuredJson = value("ai/json", true).toBool();
    p.disableThinking = value("ai/disableThinking", p.isLocal()).toBool();
    return p;
}
void Settings::saveProvider(const ProviderConfig &p) {
    set("ai/backend", translationBackendId(p.backend));
    set("ai/name", p.name);
    set("ai/url", p.baseUrl);
    set("ai/model", p.model);
    set("ai/keyEnv", p.keyEnv);
    set("ai/appId", p.appId);
    set("ai/language", p.targetLanguage);
    set("ai/timeout", p.timeoutSeconds);
    set("ai/maxTokens", p.maxTokens);
    set("ai/temperature", p.temperature);
    set("ai/json", p.structuredJson);
    set("ai/disableThinking", p.disableThinking);
}
} // namespace SnapLing

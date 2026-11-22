#pragma once
#include <QSettings>
#include <QUrl>
namespace SnapLing {
enum class TranslationBackend { OpenAiCompatible, BaiduGeneral };
QString translationBackendId(TranslationBackend backend);
TranslationBackend translationBackendFromId(const QString &id);
struct ProviderConfig {
    TranslationBackend backend = TranslationBackend::OpenAiCompatible;
    QString name = "本地 Ollama", baseUrl = "http://127.0.0.1:11434/v1", model = "qwen3.5:4b",
            keyEnv = "SNAPLING_API_KEY", appId, targetLanguage = "简体中文";
    QString sessionKey; // 仅在进程内保存，不序列化。
    int timeoutSeconds = 90, maxTokens = 4096;
    double temperature = 0.1;
    bool structuredJson = true, disableThinking = true;
    bool isLocal() const;
};
class Settings {
  public:
    explicit Settings(QString file = {});
    QVariant value(QString key, QVariant fallback = {}) const;
    bool contains(QString key) const;
    void set(QString key, QVariant value);
    ProviderConfig provider() const;
    void saveProvider(const ProviderConfig &p);
    QString fileName() const {
        return store.fileName();
    }

  private:
    mutable QSettings store;
};
} // namespace SnapLing

#pragma once
#include <QString>

namespace SnapLing::SecretStore {
bool available();
QString baiduTranslationCredential(const QString &appId = {});
QString loadBaiduTranslationSecret(const QString &appId);
QString load(const QString &name);
bool store(const QString &name, const QString &secret, QString *error = nullptr);
bool remove(const QString &name);
} // namespace SnapLing::SecretStore

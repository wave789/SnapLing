#pragma once
#include <QString>

namespace SnapLing::SecretStore {
bool available();
QString baiduTranslationCredential();
QString load(const QString &name);
bool store(const QString &name, const QString &secret);
bool remove(const QString &name);
} // namespace SnapLing::SecretStore

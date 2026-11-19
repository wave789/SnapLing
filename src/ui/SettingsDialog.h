#pragma once
#include "settings/Settings.h"
#include <QDialog>
namespace SnapLing {
class SettingsDialog final : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(Settings &settings, ProviderConfig &runtime, QWidget *parent = nullptr);
};
} // namespace SnapLing

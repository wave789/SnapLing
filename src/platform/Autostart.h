#pragma once
#include <QString>
namespace SnapLing {
class Autostart {
  public:
    static bool setEnabled(bool enabled, QString &error);
};
} // namespace SnapLing

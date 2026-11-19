#pragma once
#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>
#include <memory>
namespace SnapLing {
class IHotkeyBackend : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void bind(const QKeySequence &key) = 0;
    virtual void unbind() = 0;
  signals:
    void activated();
    void status(QString message, bool success);
};
std::unique_ptr<IHotkeyBackend> createHotkeyBackend();
} // namespace SnapLing

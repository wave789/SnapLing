#pragma once
#include "core/Types.h"
#include <QJsonArray>
namespace SnapLing {
class HistoryStore {
  public:
    explicit HistoryStore(QString directory = {});
    QJsonArray entries() const;
    bool add(const QImage &image, bool ocr, bool translation, QString &error);
    bool remove(QString id, bool files, QString &error);
    void prune(int days, qint64 maximumBytes);
    QString directory() const {
        return dir;
    }

  private:
    QString dir;
    bool write(const QJsonArray &items) const;
};
} // namespace SnapLing

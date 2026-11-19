#include "HistoryStore.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
namespace SnapLing {
HistoryStore::HistoryStore(QString path)
    : dir(path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/history"
                         : path) {}
QJsonArray HistoryStore::entries() const {
    QFile f(dir + "/index.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}
bool HistoryStore::write(const QJsonArray &items) const {
    QSaveFile f(dir + "/index.json");
    if (!f.open(QIODevice::WriteOnly))
        return false;
    auto b = QJsonDocument(items).toJson(QJsonDocument::Compact);
    return f.write(b) == b.size() && f.commit();
}
bool HistoryStore::add(const QImage &image, bool ocr, bool translation, QString &error) {
    QDir().mkpath(dir);
    QString id = QUuid::createUuid().toString(QUuid::Id128), path = dir + "/" + id + ".png";
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || !image.save(&f, "PNG") || !f.commit()) {
        error = "历史图像保存失败";
        return false;
    }
    auto items = entries();
    items.prepend(QJsonObject{{"id", id},
                              {"path", path},
                              {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                              {"width", image.width()},
                              {"height", image.height()},
                              {"ocr", ocr},
                              {"translation", translation}});
    if (!write(items)) {
        QFile::remove(path);
        error = "历史索引写入失败";
        return false;
    }
    return true;
}
bool HistoryStore::remove(QString id, bool files, QString &error) {
    auto items = entries();
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["id"].toString() == id) {
            if (files) {
                auto path = items[i].toObject()["path"].toString();
                if (QFile::exists(path) && !QFile::remove(path)) {
                    error = "无法删除历史图像";
                    return false;
                }
            }
            items.removeAt(i);
            break;
        }
    if (!write(items)) {
        error = "无法更新历史记录";
        return false;
    }
    return true;
}
void HistoryStore::prune(int days, qint64 limit) {
    auto items = entries();
    qint64 used = 0;
    auto cutoff = QDateTime::currentDateTimeUtc().addDays(-qMax(1, days));
    QJsonArray keep;
    for (auto v : items) {
        auto o = v.toObject();
        auto path = o["path"].toString();
        qint64 bytes = QFileInfo(path).size();
        if (QDateTime::fromString(o["time"].toString(), Qt::ISODate) < cutoff || used + bytes > limit) {
            QFile::remove(path);
            continue;
        }
        used += bytes;
        keep.append(v);
    }
    write(keep);
}
} // namespace SnapLing

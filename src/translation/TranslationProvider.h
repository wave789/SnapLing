#pragma once
#include "core/Types.h"
#include "settings/Settings.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
namespace SnapLing {
class ITranslationProvider : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void translate(QVector<TranslationUnit> units, ProviderConfig config) = 0;
    virtual void cancel() = 0;
  signals:
    void completed(QVector<SnapLing::TranslationUnit> units, qint64 elapsedMs);
    void failed(QString message);
};
class OpenAiTranslationProvider final : public ITranslationProvider {
    Q_OBJECT
  public:
    explicit OpenAiTranslationProvider(QObject *parent = nullptr);
    void translate(QVector<TranslationUnit>, ProviderConfig) override;
    void cancel() override;
    void testConnection(ProviderConfig config);
    static bool parse(QString content, QVector<TranslationUnit> &units, QString &error);
    static QUrl endpoint(QString base, QString path);
  signals:
    void connectionResult(bool ok, QString message);

  private:
    QNetworkAccessManager network;
    QPointer<QNetworkReply> reply;
    QTimer timeout;
    bool cancelled = false;
};
QVector<TranslationUnit> translationUnits(const QVector<OcrLine> &lines, QString language);
} // namespace SnapLing
Q_DECLARE_METATYPE(SnapLing::TranslationUnit)
Q_DECLARE_METATYPE(QVector<SnapLing::TranslationUnit>)

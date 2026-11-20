#pragma once
#include "Document.h"
#include "ocr/OcrEngine.h"
#include "settings/Settings.h"
#include "translation/TranslationProvider.h"
#include <memory>

class QWidget;
namespace SnapLing {
class EditorSession final : public QObject {
    Q_OBJECT
  private:
    std::unique_ptr<Settings> ownedSettings;
    ProviderConfig ownedConfig;

  public:
    enum class Task { Idle, Ocr, Translating };
    explicit EditorSession(QImage image = {});
    EditorSession(QImage image, Settings &settings, ProviderConfig &config);
    ~EditorSession() override;
    Settings &settings;
    ProviderConfig &config;
    Document document;
    Task task() const { return currentTask; }
    bool awaitingApproval() const { return approvalPending; }
    QString message() const { return lastMessage; }
    QString savedPath() const { return lastSavedPath; }
    QString translationEndpoint() const;
    void runOcr(bool thenTranslate = false);
    void translate(QVector<OcrLine> selected = {});
    void toggleTranslation(QVector<OcrLine> selected = {});
    bool isTranslationEnabled() const { return translationEnabled; }
    void approveTranslation(bool allowed);
    void cancel();
    void copyImage();
    void copyTranslation();
    void requestSave(QWidget *parent);
    void saveImage(QString path);

  signals:
    void statusChanged(QString message, bool error);
    void taskChanged();
    void translationApprovalRequested(QString endpoint);
    void imageSaved(QString path);
    void translationStateChanged(bool enabled);

  private:
    void initialize();
    void notify(QString message, bool error = false);
    void setTask(Task task);
    void startTranslation();
    void setTranslationEnabled(bool enabled);
    bool sourceUnchanged();
    TesseractOcr ocr;
    OpenAiTranslationProvider provider;
    Task currentTask = Task::Idle;
    bool translateAfterOcr = false, approvalPending = false;
    qint64 sourceKey = 0;
    QVector<OcrLine> translationLines;
    QVector<TranslationUnit> requestUnits;
    bool translateSelected = false;
    bool translationEnabled = false;
    int preservedLines = 0;
    QString lastMessage, lastSavedPath;
    ProviderConfig requestConfig;
};
} // namespace SnapLing

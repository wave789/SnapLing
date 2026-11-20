#pragma once
#include "editor/Canvas.h"
#include "editor/Document.h"
#include "editor/EditorSession.h"
#include "settings/Settings.h"
#include "translation/TranslationProvider.h"
#include <QDockWidget>
#include <QMainWindow>
namespace SnapLing {
class EditorWindow final : public QMainWindow {
    Q_OBJECT
  public:
    EditorWindow(QImage image, Settings &settings, ProviderConfig &config);
    explicit EditorWindow(std::shared_ptr<EditorSession> session);
    ~EditorWindow() override;
    Document *document() {
        return &doc;
    }
    Canvas *canvasWidget() {
        return canvas;
    }
    void runOcr(bool thenTranslate = false);
    void translate();
    void copy();
    bool loadProject(QString path);
  signals:
    void settingsRequested();

  private:
    void saveImage();
    void saveProject();
    void exportText();
    void sensitive();
    void requestTranslationApproval(QString endpoint);
    void editTranslation();
    std::shared_ptr<EditorSession> editing;
    Settings &settings;
    Document &doc;
    Canvas *canvas;
    QString savedPath;
    QAction *cancelAction;
    QDockWidget *comparison;
};
} // namespace SnapLing

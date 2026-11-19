#pragma once
#include "editor/Canvas.h"
#include "editor/Document.h"
#include "ocr/OcrEngine.h"
#include "settings/Settings.h"
#include "translation/TranslationProvider.h"
#include <QDockWidget>
#include <QMainWindow>
namespace SnapLing {
class EditorWindow final : public QMainWindow {
    Q_OBJECT
  public:
    EditorWindow(QImage image, Settings &settings, ProviderConfig &config);
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
    void setBusy(bool busy, QString message);
    void startTranslation();
    void editTranslation();
    Settings &settings;
    ProviderConfig &config;
    Document doc;
    Canvas *canvas;
    TesseractOcr ocr;
    OpenAiTranslationProvider provider;
    bool afterOcrTranslate = false;
    bool busy = false;
    QString savedPath;
    QAction *cancelAction;
    QDockWidget *comparison;
};
} // namespace SnapLing

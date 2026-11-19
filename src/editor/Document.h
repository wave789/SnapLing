#pragma once
#include <QPainter>

#include "core/Types.h"
#include <QColor>
#include <QFont>
#include <QObject>
#include <QTransform>
#include <functional>
namespace SnapLing {
enum class Tool {
    Select,
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Pen,
    Highlighter,
    Text,
    Number,
    Mosaic,
    Blur,
    Crop,
    Ocr,
    Picker
};
struct Annotation {
    Tool tool = Tool::Rectangle;
    QPolygonF points;
    QColor color = QColor("#ef4444");
    qreal width = 3, opacity = 1;
    QString text;
    QFont font;
    QRectF bounds() const;
};
struct DocumentState {
    QVector<Annotation> annotations;
    QVector<OcrLine> ocr;
    QVector<TranslationUnit> translations;
    QRect crop;
    int rotation = 0;
    bool flipH = false, flipV = false;
    int translationMode = 0;
    bool panel = false;
};
class Document final : public QObject {
    Q_OBJECT
  public:
    explicit Document(QImage image = {}, QObject *parent = nullptr);
    const QImage &original() const {
        return source;
    }
    const DocumentState &state() const {
        return current;
    }
    void change(std::function<void(DocumentState &)> edit);
    void undo();
    void redo();
    bool canUndo() const {
        return !past.isEmpty();
    }
    bool canRedo() const {
        return !future.isEmpty();
    }
    QImage render() const;
    QTransform imageTransform() const;
    bool saveProject(QString path, QString &error) const;
    bool loadProject(QString path, QString &error);
    bool exportImage(QString path, QString &error, QColor jpegBackground = Qt::white, int quality = 92) const;
    void reset(QImage image);
    static void paintAnnotation(QPainter &p, const QImage &source, const Annotation &a);
  signals:
    void changed();

  private:
    QImage source;
    DocumentState current;
    QVector<DocumentState> past, future;
};
} // namespace SnapLing

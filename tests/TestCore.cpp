#include "core/Types.h"
#include "editor/Document.h"
#include "history/HistoryStore.h"
#include "image/ImageProcessor.h"
#include "ocr/OcrEngine.h"
#include "ocr/TextLayout.h"
#include "selection/SelectionOverlay.h"
#include "settings/Settings.h"
#include "translation/TranslationProvider.h"
#include "ui/EditorWindow.h"
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPainter>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace SnapLing;
class TestCore : public QObject {
    Q_OBJECT
  private:
    static QImage sample() {
        QImage i(160, 100, QImage::Format_ARGB32_Premultiplied);
        i.fill(Qt::white);
        QPainter p(&i);
        p.fillRect(0, 0, 50, 40, Qt::green);
        return i;
    }
    static QVector<TranslationUnit> units() {
        return {{"a", "Hello", {}, "中文", "pending", QRectF(20, 20, 100, 30)},
                {"b", "World", {}, "中文", "pending", QRectF(20, 60, 100, 30)}};
    }
    static void serveChat(QTcpServer &server, QJsonObject response, QByteArray &received) {
        connect(&server, &QTcpServer::newConnection, &server, [&server, &received, response] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &received, response] {
                received += socket->readAll();
                auto boundary = received.indexOf("\r\n\r\n");
                if (boundary < 0)
                    return;
                QRegularExpression length("Content-Length: ([0-9]+)",
                                          QRegularExpression::CaseInsensitiveOption);
                auto match = length.match(QString::fromLatin1(received.left(boundary)));
                if (!match.hasMatch() || received.size() < boundary + 4 + match.captured(1).toInt())
                    return;
                auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
  private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<OcrLine>>();
        qRegisterMetaType<QVector<TranslationUnit>>();
        QStandardPaths::setTestModeEnabled(true);
    }
    void negativeMonitorCoordinates() {
        CaptureFrame f;
        f.desktop = {-1920, -200, 3840, 1280};
        f.image = QImage(7680, 2560, QImage::Format_RGB32);
        QCOMPARE(f.toPixel({-1920, -200}), QPointF(0, 0));
        QCOMPARE(f.toPixel({0, 0}), QPointF(3840, 400));
        QCOMPARE(f.pixelRect({-2000, -300, 180, 200}), QRect(0, 0, 200, 200));
    }
    void dpiScaling() {
        for (double scale : {1., 1.25, 1.5, 2.}) {
            CaptureFrame f;
            f.desktop = {-400, 50, 800, 600};
            f.image = QImage(qRound(800 * scale), qRound(600 * scale), QImage::Format_RGB32);
            QCOMPARE(f.toPixel({0, 350}), QPointF(400 * scale, 300 * scale));
            QCOMPARE(f.pixelRect({-400, 50, 20, 30}).size(), QSize(qRound(20 * scale), qRound(30 * scale)));
        }
    }
    void polygonMask() {
        auto image = sample();
        QPainterPath path;
        QPolygonF triangle;
        triangle << QPointF(0, 0) << QPointF(100, 0) << QPointF(50, 80);
        path.addPolygon(triangle);
        path.closeSubpath();
        auto result = maskedSelection(image, path);
        QCOMPARE(result.size(), QSize(100, 80));
        QVERIFY(result.pixelColor(50, 30).alpha() > 250);
        QCOMPARE(result.pixelColor(0, 79).alpha(), 0);
    }
    void ellipseMask() {
        QPainterPath path;
        path.addEllipse(QRectF(10, 10, 80, 60));
        auto out = maskedSelection(sample(), path);
        QCOMPARE(out.size(), QSize(80, 60));
        QCOMPARE(out.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(out.pixelColor(40, 30).alpha(), 255);
    }
    void cropRotateFlipUndo() {
        Document doc(sample());
        auto key = doc.original().cacheKey();
        doc.change([](auto &s) { s.crop = QRect(10, 10, 80, 60); });
        QCOMPARE(doc.render().size(), QSize(80, 60));
        doc.change([](auto &s) { s.rotation = 90; });
        QCOMPARE(doc.render().size(), QSize(60, 80));
        QCOMPARE(doc.imageTransform().map(QPointF(10, 10)), QPointF(60, 0));
        doc.change([](auto &s) { s.flipH = true; });
        QCOMPARE(doc.imageTransform().map(QPointF(10, 10)), QPointF(0, 0));
        doc.undo();
        QVERIFY(!doc.state().flipH);
        doc.undo();
        QCOMPARE(doc.render().size(), QSize(80, 60));
        doc.redo();
        QCOMPARE(doc.render().size(), QSize(60, 80));
        QCOMPARE(doc.original().cacheKey(), key);
    }
    void nonDestructiveAnnotations() {
        Document doc(sample());
        auto key = doc.original().cacheKey();
        Annotation a;
        a.tool = Tool::Rectangle;
        a.points << QPointF(60, 10) << QPointF(140, 80);
        a.color = Qt::red;
        a.width = 6;
        for (int i = 0; i < 40; ++i)
            doc.change([a](auto &s) { s.annotations << a; });
        QCOMPARE(doc.original().cacheKey(), key);
        QCOMPARE(doc.original().pixelColor(60, 30), QColor(Qt::white));
        QVERIFY(doc.render().pixelColor(60, 30).red() > 200);
        QCOMPARE(doc.state().annotations.size(), 40);
        doc.undo();
        QCOMPARE(doc.state().annotations.size(), 39);
        doc.redo();
        QCOMPARE(doc.state().annotations.size(), 40);
    }
    void codecsAndJpegBackground() {
        QTemporaryDir tmp;
        QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        Document d(image);
        QString error;
        QVERIFY2(d.exportImage(tmp.filePath("a.png"), error), qPrintable(error));
        QCOMPARE(QImage(tmp.filePath("a.png")).pixelColor(0, 0).alpha(), 0);
        QVERIFY(QFileInfo(tmp.filePath("a.png")).size() < 2000);
        QVERIFY(d.exportImage(tmp.filePath("a.jpg"), error, QColor("#224466"), 100));
        auto c = QImage(tmp.filePath("a.jpg")).pixelColor(30, 30);
        QVERIFY(qAbs(c.red() - 34) < 3);
        QVERIFY(qAbs(c.green() - 68) < 3);
        QVERIFY(!d.exportImage(tmp.filePath("missing/a.png"), error));
    }
    void projectRoundtripAndCorruption() {
        QTemporaryDir tmp;
        Document d(sample());
        d.change([](auto &s) {
            Annotation a;
            a.tool = Tool::Arrow;
            a.points << QPointF(3, 4) << QPointF(80, 60);
            a.font.setPixelSize(22);
            s.annotations << a;
            OcrLine l;
            l.id = "line_0";
            l.text = "中文🙂 test";
            l.box = {1, 2, 100, 30};
            l.polygon = QPolygonF(l.box);
            s.ocr << l;
            s.translations = translationUnits(s.ocr, "English");
            s.translations[0].target = "Chinese test";
            s.translations[0].status = "done";
            s.translationMode = 1;
            s.rotation = 90;
        });
        QString error;
        auto path = tmp.filePath("test.snapling");
        QVERIFY2(d.saveProject(path, error), qPrintable(error));
        Document loaded;
        QVERIFY2(loaded.loadProject(path, error), qPrintable(error));
        QCOMPARE(loaded.original(), d.original());
        QCOMPARE(loaded.render(), d.render());
        QCOMPARE(loaded.state().ocr[0].text, QString("中文🙂 test"));
        loaded.change([](auto &s) { s.annotations[0].color = Qt::blue; });
        QVERIFY(loaded.canUndo());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{broken");
        f.close();
        QVERIFY(!loaded.loadProject(path, error));
        QCOMPARE(loaded.state().ocr[0].text, QString("中文🙂 test"));
    }
    void unsupportedProjectVersion() {
        QTemporaryDir tmp;
        auto path = tmp.filePath("future.snapling");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"format\":\"SnapLing\",\"version\":99}");
        f.close();
        Document d;
        QString error;
        QVERIFY(!d.loadProject(path, error));
        QVERIFY(!error.isEmpty());
    }
    void unicodeSelection() {
        TextLayout layout;
        OcrLine a;
        a.text = "中A🙂é文";
        a.box = {0, 0, 500, 40};
        a.order = 0;
        OcrLine b;
        b.text = "第二行 hello";
        b.box = {0, 50, 500, 40};
        b.order = 1;
        layout.setLines({b, a});
        QCOMPARE(layout.characters(0).size(), 5);
        QCOMPARE(layout.selected({0, 1}, {0, 3}), QString("A🙂é"));
        QCOMPARE(layout.selected({0, 3}, {1, 1}), QString("é文\n第二"));
        QCOMPARE(layout.selected({1, 1}, {0, 3}), QString("é文\n第二"));
        auto boxes = layout.characterBoxes(0);
        QCOMPARE(layout.hit(boxes[2].center()).index, 2);
        QCOMPARE(layout.hit({900, 900}).line, -1);
        QCOMPARE(layout.selectionRects({0, 2}, {1, 2}).size(), 2);
        QCOMPARE(layout.all(), QString("中A🙂é文\n第二行 hello"));
    }
    void ocrCoordinateMapping() {
        OcrLine l;
        l.box = {10, 20, 100, 30};
        l.polygon = QPolygonF(l.box);
        QTransform t;
        t.translate(-5, 8);
        t.scale(1.5, 2);
        auto mapped = mapOcr({l}, t);
        QCOMPARE(mapped[0].box, t.mapRect(l.box));
        QCOMPARE(mapped[0].polygon, t.map(l.polygon));
    }
    void translationJsonIdMatching() {
        auto u = units();
        QString error;
        QVERIFY(OpenAiTranslationProvider::parse("```json\n{\"translations\":[{\"id\":\"b\",\"text\":"
                                                 "\"世界\"},{\"id\":\"a\",\"text\":\"你好\"}]}\n```",
                                                 u, error));
        QCOMPARE(u[0].target, QString("你好"));
        QCOMPARE(u[1].target, QString("世界"));
        QCOMPARE(u[0].status, QString("done"));
    }
    void translationJsonWithThinking() {
        auto u = units();
        QString error;
        QVERIFY2(OpenAiTranslationProvider::parse(
                     "<think>Reasoning with braces { and a fake ID.}</think>\n```json\n"
                     "{\"translations\":[{\"id\":\"a\",\"text\":\"你好\"},"
                     "{\"id\":\"b\",\"text\":\"世界\"}]}\n```", u, error),
                 qPrintable(error));
        QCOMPARE(u[0].target, QString("你好"));
        QCOMPARE(u[1].target, QString("世界"));
        auto invalid = units();
        QVERIFY(!OpenAiTranslationProvider::parse(
            "<think>{\"translations\":[{\"id\":\"a\",\"text\":\"思考内容\"}]}", invalid, error));
        QVERIFY(invalid[0].target.isEmpty());
    }
    void badTranslationIsAtomic() {
        auto u = units();
        QString e;
        for (auto input : {"garbage", "{\"translations\":[{\"id\":\"a\",\"text\":\"test\"}]}",
                           "[{\"id\":\"a\",\"text\":\"x\"},{\"id\":\"a\",\"text\":\"y\"}]",
                           "[{\"id\":\"a\",\"text\":\"x\"},{\"id\":\"unknown\",\"text\":\"y\"}]"}) {
            QVERIFY(!OpenAiTranslationProvider::parse(input, u, e));
            QVERIFY(u[0].target.isEmpty());
            QCOMPARE(u[0].source, QString("Hello"));
        }
    }
    void translationLayoutAndExport() {
        auto fit = fitTranslation("你好，世界", {10, 10, 140, 35}, {300, 200});
        QVERIFY(!fit.overflow);
        QVERIFY(fit.font.pixelSize() >= 8);
        auto longFit = fitTranslation(QString(10000, QChar(0x4e2d)), {0, 0, 10, 10}, {40, 20});
        QVERIFY(longFit.overflow);
        Document doc(sample());
        doc.change([](auto &s) {
            TranslationUnit u;
            u.id = "a";
            u.target = "中文译文";
            u.box = {55, 40, 100, 35};
            u.status = "done";
            s.translations << u;
            s.translationMode = 1;
        });
        QVERIFY(doc.render() != doc.original());
        QTemporaryDir tmp;
        QString error;
        QVERIFY(doc.exportImage(tmp.filePath("translated.png"), error));
        QCOMPARE(QImage(tmp.filePath("translated.png")).convertToFormat(QImage::Format_ARGB32_Premultiplied),
                 doc.render());
    }
    void overflowCannotSilentlyExport() {
        QTemporaryDir tmp;
        QImage image(40, 20, QImage::Format_RGB32);
        image.fill(Qt::white);
        Document d(image);
        d.change([](auto &s) {
            TranslationUnit u;
            u.id = "a";
            u.source = "source";
            u.target = QString(10000, QChar(0x4e2d));
            u.box = {0, 0, 10, 10};
            u.status = "done";
            s.translations << u;
            s.translationMode = 1;
        });
        QString error;
        QVERIFY(!d.exportImage(tmp.filePath("overflow.png"), error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(tmp.filePath("overflow.png")));
        QVERIFY(d.saveProject(tmp.filePath("complete.snapling"), error));
        Document restored;
        QVERIFY(restored.loadProject(tmp.filePath("complete.snapling"), error));
        QCOMPARE(restored.state().translations[0].target.size(), 10000);
    }
    void httpTimeout() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        OpenAiTranslationProvider provider;
        QSignalSpy failed(&provider, &ITranslationProvider::failed),
            done(&provider, &ITranslationProvider::completed);
        ProviderConfig config;
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        config.timeoutSeconds = 1;
        provider.translate(units(), config);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QVERIFY(failed[0][0].toString().contains("超时"));
        QVERIFY(done.isEmpty());
    }
    void complexBackgroundFallback() {
        QImage noise(120, 80, QImage::Format_RGB32);
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 120; ++x)
                noise.setPixelColor(x, y, ((x + y) % 2) ? Qt::black : Qt::white);
        BasicImageProcessor p;
        bool complex = false;
        p.background(noise, {20, 20, 60, 30}, complex);
        QVERIFY(complex);
        QPainter painter(&noise);
        p.cover(painter, noise, {20, 20, 60, 30}, false);
        painter.end();
        QVERIFY(noise.pixelColor(40, 30).red() > 230);
    }
    void settingsPersistenceNoSecrets() {
        QTemporaryDir tmp;
        auto path = tmp.filePath("settings.ini");
        {
            Settings settings(path);
            settings.set("hotkey/capture", "Ctrl+Shift+F8");
            ProviderConfig p;
            p.sessionKey = "secret-must-not-persist";
            p.model = "test";
            settings.saveProvider(p);
        }
        Settings settings(path);
        QCOMPARE(settings.value("schemaVersion").toInt(), 1);
        QCOMPARE(settings.provider().model, QString("test"));
        QVERIFY(settings.provider().sessionKey.isEmpty());
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("secret-must-not-persist"));
        QCOMPARE(settings.value("hotkey/capture").toString(), QString("Ctrl+Shift+F8"));
    }
    void thinkingSettings_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("defaultDisabled");
        QTest::newRow("ollama") << "http://127.0.0.1:11434/v1" << true;
        QTest::newRow("localhost") << "http://localhost:11434/v1" << true;
        QTest::newRow("ipv6") << "http://[::1]:11434/v1" << true;
        QTest::newRow("remote") << "https://example.com/v1" << false;
    }
    void thinkingSettings() {
        QFETCH(QString, url);
        QFETCH(bool, defaultDisabled);
        QTemporaryDir tmp;
        const auto path = tmp.filePath("settings.ini");
        Settings settings(path);
        // 已有设置没有新字段时，本机 Ollama 自动采用新的默认值。
        settings.set("ai/url", url);
        auto config = settings.provider();
        QCOMPARE(config.disableThinking, defaultDisabled);
        for (bool disabled : {false, true}) {
            config.disableThinking = disabled;
            settings.saveProvider(config);
            QCOMPARE(Settings(path).provider().disableThinking, disabled);
        }
    }
    void historyRetentionAndDelete() {
        QTemporaryDir tmp;
        HistoryStore h(tmp.filePath("history"));
        QString error;
        QVERIFY(h.entries().isEmpty());
        QVERIFY(h.add(sample(), false, false, error));
        QCOMPARE(h.entries().size(), 1);
        auto item = h.entries()[0].toObject();
        QVERIFY(QFile::exists(item["path"].toString()));
        QVERIFY(h.remove(item["id"].toString(), false, error));
        QVERIFY(QFile::exists(item["path"].toString()));
        QVERIFY(h.add(sample(), true, false, error));
        auto path = h.entries()[0].toObject()["path"].toString();
        h.prune(30, 1);
        QVERIFY(h.entries().isEmpty());
        QVERIFY(!QFile::exists(path));
    }
    void asynchronousHttpAndCancel_data() {
        QTest::addColumn<QString>("content");
        QTest::addColumn<QString>("reasoningField");
        QTest::addColumn<QString>("finishReason");
        QTest::addColumn<QString>("expectedError");
        QTest::addColumn<bool>("disableThinking");
        const QString translated =
            "{\"translations\":[{\"id\":\"a\",\"text\":\"你好\"},{\"id\":\"b\",\"text\":\"世界\"}]}";
        QTest::newRow("complete") << translated << "" << "stop" << "" << true;
        QTest::newRow("without-reasoning-parameter") << translated << "" << "stop" << "" << false;
        QTest::newRow("inline-thinking") << "<think>Translate each block.</think>\n" + translated
                                         << "" << "stop" << "" << true;
        QTest::newRow("complete-at-token-limit") << translated << "" << "length" << "" << true;
        QTest::newRow("reasoning-token-limit") << "" << "reasoning" << "length" << "输出上限" << true;
        QTest::newRow("truncated-json") << translated.left(40) << "" << "length" << "输出上限" << true;
        QTest::newRow("reasoning-only") << "" << "reasoning" << "stop" << "思考" << true;
        QTest::newRow("reasoning-content-only") << "" << "reasoning_content" << "stop" << "思考" << true;
        QTest::newRow("empty-content") << "" << "" << "stop" << "空" << true;
        QTest::newRow("invalid-json") << "invalid" << "" << "stop" << "JSON" << true;
    }
    void asynchronousHttpAndCancel() {
        QFETCH(QString, content);
        QFETCH(QString, reasoningField);
        QFETCH(QString, finishReason);
        QFETCH(QString, expectedError);
        QFETCH(bool, disableThinking);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray received;
        QJsonObject message{{"content", content}};
        if (!reasoningField.isEmpty())
            message[reasoningField] = "Translate the blocks.";
        serveChat(server, {{"choices", QJsonArray{QJsonObject{{"message", message},
                                                             {"finish_reason", finishReason}}}}}, received);
        OpenAiTranslationProvider provider;
        QSignalSpy done(&provider, &ITranslationProvider::completed),
            failed(&provider, &ITranslationProvider::failed);
        ProviderConfig config;
        config.disableThinking = disableThinking;
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.translate(units(), config);
        QTRY_COMPARE_WITH_TIMEOUT(done.size() + failed.size(), 1, 5000);
        if (expectedError.isEmpty()) {
            QCOMPARE(failed.size(), 0);
            auto result = qvariant_cast<QVector<TranslationUnit>>(done[0][0]);
            QCOMPARE(result[0].target, QString("你好"));
            QCOMPARE(result[1].target, QString("世界"));
        } else {
            QCOMPARE(done.size(), 0);
            QVERIFY2(failed[0][0].toString().contains(expectedError),
                     qPrintable(failed[0][0].toString()));
        }
        QVERIFY(received.startsWith("POST /v1/chat/completions"));
        auto payload = QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(payload["model"].toString(), config.model);
        if (disableThinking)
            QCOMPARE(payload["reasoning_effort"].toString(), QString("none"));
        else
            QVERIFY(!payload.contains("reasoning_effort"));
        QVERIFY(!payload.contains("tools"));
        done.clear();
        failed.clear();
        QTcpServer stall;
        QVERIFY(stall.listen(QHostAddress::LocalHost));
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(stall.serverPort());
        provider.translate(units(), config);
        provider.cancel();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(done.size(), 0);
        QVERIFY(failed[0][0].toString().contains("取消"));
    }
    void editorInPlaceTranslation() {
#ifdef SNAPLING_HAVE_TESSERACT
        QTemporaryDir tmp;
        Settings settings(tmp.filePath("settings.ini"));
        settings.set("ocr/path", findTessdata());
        settings.set("ocr/languages", "eng");
        ProviderConfig config;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        QByteArray received;
        serveChat(server, {{"choices", QJsonArray{QJsonObject{
            {"message", QJsonObject{{"content", "<think>Translate.</think>\n"
                "{\"translations\":[{\"id\":\"line_0\",\"text\":\"你好，世界\"}]}"}}},
            {"finish_reason", "stop"}}}}}, received);
        QImage image(600, 160, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        QFont font("Noto Sans CJK SC");
        font.setPixelSize(40);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRect(20, 30, 560, 80), Qt::AlignLeft | Qt::AlignVCenter, "Hello world");
        painter.end();
        EditorWindow editor(image, settings, config);
        QString errorDialog;
        QTimer dismissErrors;
        connect(&dismissErrors, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                    errorDialog = dialog->text();
                    dialog->accept();
                }
        });
        dismissErrors.start(20);
        QAction *translate = nullptr;
        for (auto *action : editor.findChildren<QAction *>())
            if (action->text() == "原地翻译")
                translate = action;
        QVERIFY(translate);
        translate->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!editor.document()->state().translations.isEmpty() || !errorDialog.isEmpty(),
                                 10000);
        QVERIFY2(errorDialog.isEmpty(), qPrintable(errorDialog));
        QCOMPARE(editor.document()->state().translations.size(), 1);
        QCOMPARE(editor.document()->state().translationMode, 1);
        QCOMPARE(editor.document()->state().translations[0].target, QString("你好，世界"));
        QVERIFY(editor.document()->render() != editor.document()->original());
        QString error;
        const auto output = tmp.filePath("translated.png");
        QVERIFY2(editor.document()->exportImage(output, error), qPrintable(error));
        QCOMPARE(QImage(output).convertToFormat(QImage::Format_ARGB32_Premultiplied),
                 editor.document()->render());
#else
        QSKIP("此构建未包含 OCR 引擎");
#endif
    }
    void realOfflineOcr() {
#ifdef SNAPLING_HAVE_TESSERACT
        if (findTessdata().isEmpty())
            QSKIP("未安装 OCR 模型");
        QImage image(1000, 240, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        QFont font("Noto Sans CJK SC");
        font.setPixelSize(48);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(30, 80, "Hello SnapLing 123");
        painter.drawText(30, 170, "中文识别测试 你好世界");
        painter.end();
        TesseractOcr engine;
        QSignalSpy done(&engine, &IOcrEngine::completed), failed(&engine, &IOcrEngine::failed);
        engine.recognize(image, findTessdata(), "chi_sim+eng", true);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 50000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
        auto lines = qvariant_cast<QVector<OcrLine>>(done[0][0]);
        TextLayout layout;
        layout.setLines(lines);
        QVERIFY2(layout.all().contains("Hello"), qPrintable(layout.all()));
        QVERIFY2(layout.all().contains("中文"), qPrintable(layout.all()));
        for (auto line : lines) {
            QVERIFY(line.box.width() > 0);
            QVERIFY(line.estimatedCharacters);
        }
        qInfo("ocr_test_cold_ms=%lld", done[0][1].toLongLong());
        engine.recognize(image, findTessdata(), "chi_sim+eng", true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 50000);
        QVERIFY(!done[1][2].toBool());
        qInfo("ocr_test_warm_ms=%lld", done[1][1].toLongLong());
#else
        QSKIP("本构建禁用 OCR");
#endif
    }
};
QTEST_MAIN(TestCore)
#include "TestCore.moc"

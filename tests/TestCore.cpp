#include "core/Types.h"
#include "capture/CaptureService.h"
#include "editor/Document.h"
#include "editor/EditorSession.h"
#include "history/HistoryStore.h"
#include "image/ImageProcessor.h"
#include "ocr/OcrEngine.h"
#include "ocr/RapidOcr.h"
#include "ocr/TextLayout.h"
#include "platform/SecretStore.h"
#include "selection/SelectionOverlay.h"
#include "settings/Settings.h"
#include "translation/TranslationProvider.h"
#include "ui/EditorWindow.h"
#include "ui/SettingsDialog.h"
#ifndef Q_OS_WIN
#include "platform/linux/wayland/WaylandCapture.h"
#endif
#include <QAction>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>
using namespace SnapLing;
class TestCore : public QObject {
    Q_OBJECT
  private:
    static bool installSecretTool(const QTemporaryDir &directory) {
        QFile tool(directory.filePath("secret-tool"));
        if (!tool.open(QIODevice::WriteOnly))
            return false;
        const auto script = QString(
            "#!/bin/sh\n"
            "state='%1'\n"
            "[ ! -f \"$state/fail-keyring\" ] || exit 1\n"
            "for attribute do credential=\"$attribute\"; done\n"
            "case \"$1\" in\n"
            "  store) cat > \"$state/$credential\" ;;\n"
            "  lookup) [ -f \"$state/$credential\" ] && cat \"$state/$credential\" ;;\n"
            "  clear) rm -f \"$state/$credential\" ;;\n"
            "esac\n").arg(directory.path());
        if (tool.write(script.toUtf8()) != script.toUtf8().size())
            return false;
        tool.close();
        return QFile::setPermissions(tool.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                       QFileDevice::ExeOwner);
    }
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
    void mixedDpiScreenComposition() {
        QVector<CaptureScreen> screens{{"left", {-100, 0, 100, 60}, {}, 1},
                                       {"internal", {0, 0, 80, 50}, {}, 1.5},
                                       {"right", {80, -20, 60, 50}, {}, 2}};
        const QList<QColor> colors{Qt::red, Qt::green, Qt::blue};
        for (int i = 0; i < screens.size(); ++i) {
            screens[i].image = QImage((screens[i].desktop.size() * screens[i].devicePixelRatio).toSize(), QImage::Format_RGB32);
            screens[i].image.fill(colors[i]);
        }
        QString error;
        const auto frame = combineCaptureScreens(screens, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(frame.desktop, QRectF(-100, -20, 240, 80));
        QCOMPARE(frame.image.size(), QSize(480, 160));
        QCOMPARE(frame.screens.size(), 3);
        for (int i = 0; i < screens.size(); ++i) {
            QCOMPARE(frame.screens[i].image, screens[i].image);
            QCOMPARE(frame.image.pixelColor(frame.toPixel(screens[i].desktop.center()).toPoint()), colors[i]);
        }
        QCOMPARE(frame.image.pixelColor(frame.toPixel({-90, -10}).toPoint()).alpha(), 0);
    }
    void singleScreenPreservesNativePixels() {
        QImage image(300, 180, QImage::Format_RGB32);
        image.fill(Qt::cyan);
        image.setPixelColor(23, 37, Qt::red);
        QString error;
        const auto frame = combineCaptureScreens({{"external", {-200, 50, 150, 90}, image, 2}}, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(frame.image, image);
        QCOMPARE(frame.screenName, QString("external"));
        QCOMPARE(frame.toPixel({-188.5, 68.5}), QPointF(23, 37));
    }
    void invalidScreenCompositionIsRejected() {
        QString error;
        QVERIFY(combineCaptureScreens({}, error).image.isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(combineCaptureScreens({{"empty", {0, 0, 100, 100}, {}}}, error).image.isNull());
        QVERIFY(!error.isEmpty());
        QImage image(1, 1, QImage::Format_RGB32);
        QVERIFY(combineCaptureScreens({{"huge", {0, 0, 20000, 20000}, image}}, error).image.isNull());
        QVERIFY(error.contains("像素限制"));
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
    void rawTextureBmpCanBeDecoded() {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        const quint16 magic = 0x4d42;
        const quint32 fileSize = 54 + 16, offset = 54, infoSize = 40, imageSize = 16;
        const qint32 width = 2, height = -2, resolution = 2835;
        stream << magic << fileSize << quint32(0) << offset << infoSize << width << height;
        stream << quint16(1) << quint16(32) << quint32(0) << imageSize << resolution << resolution;
        stream << quint32(0) << quint32(0);
        QVERIFY(stream.status() == QDataStream::Ok);
        const QByteArray pixels = QByteArray::fromHex("0000ffff00ff00ffff0000ffffffffff");
        QCOMPARE(pixels.size(), 16);
        bytes += pixels;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
        const auto image = reader.read();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(2, 2));
    }
    void rawTextureContainerCanBeDecoded() {
#ifndef Q_OS_WIN
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("SNRAW1", 6);
        stream << quint16(1) << qint32(2) << qint32(2) << qint32(8) << quint32(2) << quint64(16);
        const QByteArray pixels = QByteArray::fromHex("0000ff0000ff0000000000ff00000000");
        QCOMPARE(pixels.size(), 16);
        bytes += pixels;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QString error;
        const auto image = WaylandCapture::decodeCaptureImage(buffer, error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QCOMPARE(image.size(), QSize(2, 2));
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(image.pixelColor(1, 0), QColor(Qt::green));
#else
        QSKIP("Wayland raw texture container is Linux-only");
#endif
    }
    void legacyRawTextureAlphaDoesNotHideScreenshot() {
#ifndef Q_OS_WIN
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("SNRAW1", 6);
        stream << quint16(1) << qint32(2) << qint32(1) << qint32(8) << quint32(1) << quint64(8);
        const QByteArray pixels = QByteArray::fromHex("0000ffff00ff0000");
        bytes += pixels;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QString error;
        const auto image = WaylandCapture::decodeCaptureImage(buffer, error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(image.pixelColor(1, 0), QColor(Qt::green));
#else
        QSKIP("Wayland raw texture container is Linux-only");
#endif
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
    void baiduResponseParsingAndErrors() {
        QVector<TranslationUnit> units{
            {"a", "Hello", {}, "zh", "pending", {10, 10, 100, 20}},
            {"b", "World", {}, "zh", "pending", {10, 40, 100, 20}}};
        QString error;
        QVERIFY2(BaiduTranslationProvider::parseResponse(
                     R"({"from":"en","to":"zh","trans_result":[{"src":"Hello","dst":"你好"},{"src":"World","dst":"世界"}]})",
                     units, error),
                 qPrintable(error));
        QCOMPARE(units[0].target, QString("你好"));
        QCOMPARE(units[1].target, QString("世界"));
        QCOMPARE(units[0].status, QString("done"));
        QVERIFY(!BaiduTranslationProvider::parseResponse(
            R"({"error_code":"54001","error_msg":"Invalid Sign"})", units, error));
        QVERIFY(error.contains("签名"));
        QCOMPARE(BaiduTranslationProvider::languageCode("简体中文"), QString("zh"));
        QCOMPARE(BaiduTranslationProvider::languageCode("English"), QString("en"));
        QCOMPARE(BaiduTranslationProvider::languageCode("日语"), QString("jp"));
    }
    void baiduRequestSigningAndTranslation_data() {
        QTest::addColumn<QString>("credentialSource");
        QTest::newRow("entered-key-over-stale-environment") << "entered";
        QTest::newRow("saved-key-after-settings-reload") << "saved";
        QTest::newRow("environment-without-saved-key") << "environment";
    }
    void baiduRequestSigningAndTranslation() {
        QFETCH(QString, credentialSource);
        QTemporaryDir tmp;
#ifdef Q_OS_WIN
        if (credentialSource == "saved")
            QSKIP("系统密钥环测试使用 Linux secret-tool");
#else
        QVERIFY(installSecretTool(tmp));
#endif
        const auto oldPath = qgetenv("PATH");
        const auto oldEnvironment = qgetenv("SNAPLING_TEST_BAIDU_KEY");
        const auto restore = qScopeGuard([oldPath, oldEnvironment] {
            qputenv("PATH", oldPath);
            if (oldEnvironment.isNull())
                qunsetenv("SNAPLING_TEST_BAIDU_KEY");
            else
                qputenv("SNAPLING_TEST_BAIDU_KEY", oldEnvironment);
        });
#ifndef Q_OS_WIN
        qputenv("PATH", (tmp.path() + ":" + QString::fromUtf8(oldPath)).toUtf8());
#endif
        qputenv("SNAPLING_TEST_BAIDU_KEY", credentialSource == "environment" ? "secret-key" : "stale-key");
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QString signatureError;
        QByteArray receivedAppId;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            socket->setProperty("request", QByteArray{});
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", bytes);
                const int boundary = bytes.indexOf("\r\n\r\n");
                if (boundary < 0 || socket->property("responded").toBool())
                    return;
                const auto headers = bytes.left(boundary);
                auto match = QRegularExpression("Content-Length: ([0-9]+)",
                                                QRegularExpression::CaseInsensitiveOption)
                                 .match(QString::fromLatin1(headers));
                if (!match.hasMatch() || bytes.size() < boundary + 4 + match.captured(1).toInt())
                    return;
                socket->setProperty("responded", true);
                const auto body = bytes.mid(boundary + 4, match.captured(1).toInt());
                const QUrlQuery form(QString::fromUtf8(body));
                receivedAppId = form.queryItemValue("appid", QUrl::FullyDecoded).toUtf8();
                const auto query = form.queryItemValue("q", QUrl::FullyDecoded);
                const auto salt = form.queryItemValue("salt", QUrl::FullyDecoded);
                const auto sign = form.queryItemValue("sign", QUrl::FullyDecoded);
                const auto expected = QString::fromLatin1(
                    QCryptographicHash::hash(("app-id" + query + salt + "secret-key").toUtf8(),
                                             QCryptographicHash::Md5)
                        .toHex());
                if (query != "Hello\nWorld" || form.queryItemValue("from") != "auto" ||
                    form.queryItemValue("to") != "zh" || sign != expected)
                    signatureError = "Bad Baidu form: " + query + " / " + sign;
                const auto response = QByteArray(
                    R"({"from":"en","to":"zh","trans_result":[{"src":"Hello","dst":"你好"},{"src":"World","dst":"世界"}]})");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        ProviderConfig config;
        config.backend = TranslationBackend::BaiduGeneral;
        config.baseUrl = QString("http://127.0.0.1:%1/translate").arg(server.serverPort());
        config.appId = "app-id";
        config.keyEnv = "SNAPLING_TEST_BAIDU_KEY";
        if (credentialSource == "entered")
            config.sessionKey = "secret-key";
        if (credentialSource == "saved") {
            QVERIFY(SecretStore::store(SecretStore::baiduTranslationCredential(config.appId), "secret-key"));
            Settings settings(tmp.filePath("restart.ini"));
            settings.saveProvider(config);
            config = Settings(settings.fileName()).provider();
            QVERIFY(config.sessionKey.isEmpty());
        }
        config.targetLanguage = "简体中文";
        BaiduTranslationProvider provider;
        QSignalSpy done(&provider, &ITranslationProvider::completed);
        QSignalSpy errors(&provider, &ITranslationProvider::failed);
        provider.translate(units(), config);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !errors.isEmpty(), 5000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QVERIFY2(signatureError.isEmpty(), qPrintable(signatureError));
        QCOMPARE(receivedAppId, QByteArray("app-id"));
        const auto translated = qvariant_cast<QVector<TranslationUnit>>(done.first().first());
        QCOMPARE(translated.size(), 2);
        QCOMPARE(translated[0].target, QString("你好"));
        QCOMPARE(translated[1].target, QString("世界"));
    }
    void baiduTranslationDoesNotRequestSecondConfirmation() {
        QTemporaryDir tmp;
        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        config.backend = TranslationBackend::BaiduGeneral;
        config.appId = "configured-app-id";
        config.baseUrl = "http://127.0.0.1:1/translate";
        config.keyEnv.clear();
        config.sessionKey.clear();
        QImage image(300, 80, QImage::Format_RGB32);
        image.fill(Qt::white);
        EditorSession session(image, settings, config);
        session.document.change([](auto &state) {
            state.ocr = {{"line_0", "Translate this", {10, 10, 180, 20}, {}, .9}};
        });
        QSignalSpy approval(&session, &EditorSession::translationApprovalRequested);
        session.toggleTranslation();
        QCOMPARE(approval.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(session.task(), EditorSession::Task::Idle, 4000);
    }
    void automaticTranslationConfidenceThresholdIsQuarter() {
        const QVector<OcrLine> lines{
            {"below", "Below threshold text", {10, 10, 180, 20}, {}, .249},
            {"at-threshold", "At threshold text", {10, 40, 180, 20}, {}, .25},
            {"above", "Above threshold text", {10, 70, 180, 20}, {}, .3}};
        const auto units = translationUnits(lines, "简体中文", true);
        QCOMPARE(units.size(), 2);
        QCOMPARE(units[0].id, QString("at-threshold"));
        QCOMPARE(units[1].id, QString("above"));
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
        auto tight = fitTranslation("打开项目", {20, 30, 100, 12}, {300, 200}, 12);
        QVERIFY(!tight.overflow);
        QCOMPARE(tight.font.pixelSize(), 12);
        QVERIFY(tight.box.height() >= 12);
        QCOMPARE(tight.box.top(), 30.);
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
            p.backend = TranslationBackend::BaiduGeneral;
            p.appId = "baidu-app-id";
            p.sessionKey = "secret-must-not-persist";
            p.model = "test";
            settings.saveProvider(p);
        }
        Settings settings(path);
        QCOMPARE(settings.value("schemaVersion").toInt(), 2);
        QCOMPARE(settings.provider().model, QString("test"));
        QCOMPARE(settings.provider().backend, TranslationBackend::BaiduGeneral);
        QCOMPARE(settings.provider().appId, QString("baidu-app-id"));
        QVERIFY(settings.provider().sessionKey.isEmpty());
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("secret-must-not-persist"));
        QCOMPARE(settings.value("hotkey/capture").toString(), QString("Ctrl+Shift+F8"));
    }
    void baiduProviderCanBeSavedWithoutPersistingSecret() {
#ifdef Q_OS_WIN
        QSKIP("系统密钥环测试使用 Linux secret-tool");
#endif
        QTemporaryDir tmp;
        QVERIFY(installSecretTool(tmp));
        const auto oldPath = qEnvironmentVariable("PATH");
        const auto restore = qScopeGuard([oldPath] { qputenv("PATH", oldPath.toUtf8()); });
        qputenv("PATH", (tmp.path() + ":" + oldPath).toUtf8());
        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        SettingsDialog dialog(settings, config);
        auto *backend = dialog.findChild<QComboBox *>("translationBackend");
        auto *appId = dialog.findChild<QLineEdit *>("baiduAppId");
        auto *name = dialog.findChild<QLineEdit *>("translationProviderName");
        auto *env = dialog.findChild<QLineEdit *>("translationKeyEnv");
        auto *secret = dialog.findChild<QLineEdit *>("translationSecret");
        auto *profiles = dialog.findChild<QComboBox *>("translationProfiles");
        QVERIFY(backend && appId && name && env && secret && profiles);
        const int index = backend->findData(int(TranslationBackend::BaiduGeneral));
        QVERIFY(index >= 0);
        backend->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(backend, "activated", Qt::DirectConnection, Q_ARG(int, index)));
        name->setText("我的百度翻译");
        appId->setText("saved-app-id");
        env->setText("BAIDU_TRANSLATE_SECRET_KEY");
        secret->setText("session-secret");
        QPushButton *remember = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == "添加 / 更新供应商")
                remember = button;
        QVERIFY(remember);
        remember->click();
        QCOMPARE(profiles->count(), 1);

        appId->clear();
        profiles->setCurrentIndex(0);
        QVERIFY(QMetaObject::invokeMethod(profiles, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
        QCOMPARE(appId->text(), QString("saved-app-id"));
        secret->setText("session-secret");
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Save)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(config.backend, TranslationBackend::BaiduGeneral);
        QCOMPARE(config.appId, QString("saved-app-id"));
        QCOMPARE(config.sessionKey, QString("session-secret"));
        QCOMPARE(Settings(tmp.filePath("settings.ini")).provider().appId, QString("saved-app-id"));
        QVERIFY(Settings(tmp.filePath("settings.ini")).provider().sessionKey.isEmpty());
        auto restored = Settings(tmp.filePath("settings.ini")).provider();
        SettingsDialog restarted(settings, restored);
        QCOMPARE(restarted.findChild<QLineEdit *>("translationSecret")->text(), QString("session-secret"));
        QVERIFY(SecretStore::store(SecretStore::baiduTranslationCredential("another-app"), "another-secret"));
        QCOMPARE(SecretStore::loadBaiduTranslationSecret("saved-app-id"), QString("session-secret"));
        QCOMPARE(SecretStore::loadBaiduTranslationSecret("another-app"), QString("another-secret"));
    }
    void baiduSaveFailureKeepsDialogOpenAndPreservesSettings() {
#ifdef Q_OS_WIN
        QSKIP("系统密钥环测试使用 Linux secret-tool");
#endif
        QTemporaryDir tmp;
        QVERIFY(installSecretTool(tmp));
        const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([oldPath] { qputenv("PATH", oldPath); });
        qputenv("PATH", (tmp.path() + ":" + QString::fromUtf8(oldPath)).toUtf8());
        QFile lock(tmp.filePath("fail-keyring"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();
        QString error;
        QVERIFY(!SecretStore::store(SecretStore::baiduTranslationCredential("app"), "new-secret", &error));
        QVERIFY(!error.isEmpty());

        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        settings.saveProvider(config);
        SettingsDialog dialog(settings, config);
        auto *backend = dialog.findChild<QComboBox *>("translationBackend");
        const int index = backend->findData(int(TranslationBackend::BaiduGeneral));
        backend->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(backend, "activated", Qt::DirectConnection, Q_ARG(int, index)));
        dialog.findChild<QLineEdit *>("baiduAppId")->setText("app");
        dialog.findChild<QLineEdit *>("translationSecret")->setText("new-secret");
        dialog.show();
        QString warning;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                    warning = message->text();
                    message->accept();
                }
        });
        dismiss.start(20);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(!warning.isEmpty());
        QVERIFY(dialog.isVisible());
        QCOMPARE(config.backend, TranslationBackend::OpenAiCompatible);
        QCOMPARE(Settings(settings.fileName()).provider().backend, TranslationBackend::OpenAiCompatible);
        QVERIFY(QFile::remove(lock.fileName()));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(SecretStore::loadBaiduTranslationSecret("app"), QString("new-secret"));
    }
    void systemCredentialStoreRoundTrip() {
#ifdef Q_OS_WIN
        QSKIP("系统密钥环测试使用 Linux secret-tool");
#endif
        QTemporaryDir tmp;
        QVERIFY(installSecretTool(tmp));
        const auto oldPath = qEnvironmentVariable("PATH");
        const auto restore = qScopeGuard([oldPath] { qputenv("PATH", oldPath.toUtf8()); });
        qputenv("PATH", (tmp.path() + ":" + oldPath).toUtf8());
        QVERIFY(SecretStore::available());
        QVERIFY(SecretStore::store("test-key", "encrypted-by-system"));
        QCOMPARE(SecretStore::load("test-key"), QString("encrypted-by-system"));
        QVERIFY(SecretStore::store("baidu-general-secret", "cached-baidu-key"));
        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        config.backend = TranslationBackend::BaiduGeneral;
        SettingsDialog dialog(settings, config);
        auto *secret = dialog.findChild<QLineEdit *>("translationSecret");
        QVERIFY(secret);
        QCOMPARE(secret->text(), QString("cached-baidu-key"));
        QVERIFY(SecretStore::remove("test-key"));
        QVERIFY(SecretStore::load("test-key").isEmpty());
    }
    void baiduTranslationLoadsKeyringAfterRestart() {
#ifdef Q_OS_WIN
        QSKIP("系统密钥环测试使用 Linux secret-tool");
#endif
        QTemporaryDir tmp;
        QVERIFY(installSecretTool(tmp));
        const auto oldPath = qEnvironmentVariable("PATH");
        const auto oldSecret = qEnvironmentVariable("BAIDU_TRANSLATE_SECRET_KEY");
        const auto restore = qScopeGuard([oldPath, oldSecret] {
            qputenv("PATH", oldPath.toUtf8());
            if (oldSecret.isEmpty())
                qunsetenv("BAIDU_TRANSLATE_SECRET_KEY");
            else
                qputenv("BAIDU_TRANSLATE_SECRET_KEY", oldSecret.toUtf8());
        });
        qputenv("PATH", (tmp.path() + ":" + oldPath).toUtf8());
        qunsetenv("BAIDU_TRANSLATE_SECRET_KEY");
        QVERIFY(SecretStore::store(SecretStore::baiduTranslationCredential(), "cached-secret"));

        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        config.backend = TranslationBackend::BaiduGeneral;
        config.appId = "configured-app";
        config.baseUrl = "http://127.0.0.1:1/translate";
        config.keyEnv = "BAIDU_TRANSLATE_SECRET_KEY";
        config.timeoutSeconds = 1;
        QImage image(300, 80, QImage::Format_RGB32);
        image.fill(Qt::white);
        EditorSession session(image, settings, config);
        session.document.change([](auto &state) {
            state.ocr = {{"line_0", "Translate this", {10, 10, 180, 20}, {}, .9}};
        });
        session.toggleTranslation();
        QTRY_VERIFY_WITH_TIMEOUT(session.config.sessionKey == QString("cached-secret"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(session.task(), EditorSession::Task::Idle, 3000);
        QVERIFY(!session.message().contains("请填写百度翻译 AppID 和密钥"));
    }
    void baiduSettingsTestButtonUsesEnteredCredentials() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray receivedAppId;
        QString signatureError;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            socket->setProperty("request", QByteArray{});
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", bytes);
                const int boundary = bytes.indexOf("\r\n\r\n");
                if (boundary < 0 || socket->property("responded").toBool())
                    return;
                const auto match = QRegularExpression("Content-Length: ([0-9]+)",
                                                      QRegularExpression::CaseInsensitiveOption)
                                       .match(QString::fromLatin1(bytes.left(boundary)));
                if (!match.hasMatch() || bytes.size() < boundary + 4 + match.captured(1).toInt())
                    return;
                socket->setProperty("responded", true);
                const auto body = bytes.mid(boundary + 4, match.captured(1).toInt());
                const QUrlQuery form(QString::fromUtf8(body));
                receivedAppId = form.queryItemValue("appid", QUrl::FullyDecoded).toUtf8();
                const auto expected = QString::fromLatin1(
                    QCryptographicHash::hash(("ui-app-id" + form.queryItemValue("q", QUrl::FullyDecoded) +
                                              form.queryItemValue("salt", QUrl::FullyDecoded) +
                                              "ui-secret")
                                                 .toUtf8(),
                                             QCryptographicHash::Md5)
                        .toHex());
                if (form.queryItemValue("sign", QUrl::FullyDecoded) != expected)
                    signatureError = "UI test request did not use the entered secret";
                const auto response =
                    QByteArray(R"({"from":"en","to":"zh","trans_result":[{"src":"Hello","dst":"你好"}]})");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        const auto previousEndpoint = qEnvironmentVariable("SNAPLING_BAIDU_ENDPOINT");
        const auto restore = qScopeGuard([previousEndpoint] {
            if (previousEndpoint.isEmpty())
                qunsetenv("SNAPLING_BAIDU_ENDPOINT");
            else
                qputenv("SNAPLING_BAIDU_ENDPOINT", previousEndpoint.toUtf8());
        });
        qputenv("SNAPLING_BAIDU_ENDPOINT",
                QString("http://127.0.0.1:%1/translate").arg(server.serverPort()).toUtf8());
        QTemporaryDir tmp;
        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        SettingsDialog dialog(settings, config);
        auto *backend = dialog.findChild<QComboBox *>("translationBackend");
        auto *appId = dialog.findChild<QLineEdit *>("baiduAppId");
        auto *secret = dialog.findChild<QLineEdit *>("translationSecret");
        QVERIFY(backend && appId && secret);
        const int index = backend->findData(int(TranslationBackend::BaiduGeneral));
        backend->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(backend, "activated", Qt::DirectConnection, Q_ARG(int, index)));
        appId->setText("ui-app-id");
        secret->setText("ui-secret");
        QMessageBox *messageBox = nullptr;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                    messageBox = box;
                    box->accept();
                }
        });
        dismiss.start(20);
        QPushButton *test = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == "测试连接")
                test = button;
        QVERIFY(test);
        test->click();
        QTRY_COMPARE_WITH_TIMEOUT(receivedAppId, QByteArray("ui-app-id"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(messageBox != nullptr, 5000);
        QVERIFY2(signatureError.isEmpty(), qPrintable(signatureError));
    }
    void legacyScreenshotMigratesToInstantSelection() {
        QTemporaryDir tmp;
        auto path = tmp.filePath("legacy.ini");
        {
            QSettings legacy(path, QSettings::IniFormat);
            legacy.setValue("schemaVersion", 1);
            legacy.setValue("capture/waylandMethod", "screenshot");
        }
        const auto desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
        const auto session = qEnvironmentVariable("XDG_SESSION_TYPE");
        qputenv("XDG_CURRENT_DESKTOP", "GNOME");
        qputenv("XDG_SESSION_TYPE", "wayland");
        const auto restore = qScopeGuard([desktop, session] {
            qputenv("XDG_CURRENT_DESKTOP", desktop.toUtf8());
            qputenv("XDG_SESSION_TYPE", session.toUtf8());
        });
        Settings settings(path);
        QCOMPARE(settings.value("schemaVersion").toInt(), 2);
        QCOMPARE(settings.value("capture/waylandMethod").toString(), QString("compositor"));
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
        qputenv("SNAPLING_OCR_TEST_ENGINE", "tesseract");
        const auto restoreOcrEngine =
            qScopeGuard([] { qunsetenv("SNAPLING_OCR_TEST_ENGINE"); });
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        Settings settings(tmp.filePath("settings.ini"));
        settings.set("ocr/path", tmp.filePath("old-checkout/models/tessdata"));
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
    void ocrModelLookupRequiresAllRequestedLanguages() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile english(temp.filePath("eng.traineddata"));
        QVERIFY(english.open(QIODevice::WriteOnly));
        QCOMPARE(english.write("fixture"), 7);
        english.close();
        const auto previousPrefix = qgetenv("TESSDATA_PREFIX");
        const auto restorePrefix = qScopeGuard([previousPrefix] {
            if (previousPrefix.isNull())
                qunsetenv("TESSDATA_PREFIX");
            else
                qputenv("TESSDATA_PREFIX", previousPrefix);
        });
        qunsetenv("TESSDATA_PREFIX");
        const auto defaultMixedModels = findTessdata("chi_sim+eng");
        qputenv("TESSDATA_PREFIX", QFile::encodeName(temp.path()));
        const auto userEnglishModel = availableTessdata(userTessdataDirectory(), "eng");
        QCOMPARE(findTessdata("eng"), userEnglishModel.isEmpty() ? QDir::cleanPath(temp.path()) : userEnglishModel);
        QCOMPARE(findTessdata("chi_sim+eng"), defaultMixedModels);
        const auto homeRelative = QDir(QDir::homePath()).relativeFilePath(temp.path());
        if (QDir::isRelativePath(homeRelative))
            QCOMPARE(availableTessdata("~/" + homeRelative, "eng"), QDir::cleanPath(temp.path()));
        QVERIFY(availableTessdata(temp.path(), "chi_sim+eng").isEmpty());
        QVERIFY(availableTessdata(temp.filePath("missing"), "eng").isEmpty());
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
        engine.recognize(image, {}, "chi_sim+eng", true);
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
    void paddleOcrIsFixedAndBundled() {
#ifndef Q_OS_WIN
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        Settings settings(tmp.filePath("settings.ini"));
        ProviderConfig config;
        SettingsDialog dialog(settings, config);
        QVERIFY(!dialog.findChild<QComboBox *>("ocrEngine"));
        auto *status = dialog.findChild<QLabel *>("paddleOcrStatus");
        QVERIFY(status);
        QVERIFY(status->text().contains("安装包内置") || status->text().contains("随 SnapLing 安装"));
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Save)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(Settings(tmp.filePath("settings.ini")).value("ocr/engine").toString(), QString("paddleocr"));
        QVERIFY(QFileInfo(RapidOcr::modelDirectory() + "/det/inference.yml").isFile());
        QVERIFY(QFileInfo(RapidOcr::modelDirectory() + "/rec/inference.yml").isFile());
        if (!qEnvironmentVariableIsSet("SNAPLING_TEST_PADDLEOCR"))
            return;
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
        EditorSession session(image, settings, config);
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression("ocr_ms=\\d+ cold=\\d lines=\\d+ engine=paddleocr"));
        session.runOcr();
        QTRY_COMPARE_WITH_TIMEOUT(session.task(), EditorSession::Task::Idle, 120000);
        const auto &lines = session.document.state().ocr;
        QVERIFY2(!lines.isEmpty(), qPrintable(session.message()));
        TextLayout layout;
        layout.setLines(lines);
        QVERIFY2(layout.all().contains("Hello"), qPrintable(layout.all()));
        QVERIFY2(layout.all().contains("中文"), qPrintable(layout.all()));
        for (const auto &line : lines) {
            QVERIFY(QRectF(image.rect()).contains(line.box));
            QVERIFY(line.polygon.size() >= 3);
        }
#else
        QSKIP("PaddleOCR runtime is currently packaged for Linux");
#endif
    }
};
QTEST_MAIN(TestCore)
#include "TestCore.moc"

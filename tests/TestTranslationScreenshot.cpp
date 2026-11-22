#include "editor/EditorSession.h"
#include "image/ImageProcessor.h"
#include "ocr/TextLayout.h"
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegion>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace SnapLing;

// 重建与问题截图相同的深色菜单、多列文字和小字号路径，不含用户截图内容。
static QImage desktopFixture(bool dark, qreal scale = 1) {
    QImage image(1096, 768, QImage::Format_RGB32);
    image.fill(dark ? QColor("#181a1c") : QColor("#e7e9ed"));
    QPainter p(&image);
    QFont font("Noto Sans CJK SC");
    font.setPixelSize(22);
    p.setFont(font);
    p.setPen(dark ? QColor("#bfc1c5") : QColor("#272b33"));
    for (int y = 190; y < 700; y += 65)
        p.drawText(255, y, "Select a region to translate text without changing the original image.");
    p.fillRect(78, 38, 774, 680, dark ? QColor("#25272b") : QColor("#fafafa"));
    font.setPixelSize(16);
    p.setFont(font);
    p.drawText(110, 64, "打开...");
    p.drawText(110, 89, "克隆仓库...");
    p.drawText(110, 127, "PyCharm Home");
    p.drawText(95, 172, "打开项目");
    p.drawText(120, 206, "study");
    p.drawText(120, 273, "SnapLing");
    p.drawText(95, 344, "最近的项目");
    const QStringList names{"19.ECommerceServiceAI", "siyuan-search", "yolov3_inference", "MinerU", "minimind"};
    QFont path = font;
    path.setPixelSize(14);
    p.setFont(path);
    p.setPen(dark ? QColor("#73767d") : QColor("#666b73"));
    p.drawText(120, 233, "~/D/workspace/Projects/study");
    p.drawText(120, 300, "~/D/workspace/Projects/SnapLing");
    for (int index = 0; index < names.size(); ++index) {
        const int y = 375 + index * 67;
        p.setFont(font);
        p.setPen(dark ? QColor("#c8cacf") : QColor("#272b33"));
        p.drawText(120, y, names[index]);
        p.setPen(dark ? QColor("#73767d") : QColor("#666b73"));
        p.setFont(path);
        p.drawText(120, y + 27, "~/D/workspace/Projects/" + names[index]);
    }
    p.end();
    if (scale != 1)
        image = image.scaled(qRound(image.width() * scale), qRound(image.height() * scale),
                              Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image.setDevicePixelRatio(scale);
    return image;
}
class TestTranslationScreenshot : public QObject {
    Q_OBJECT
  private slots:
    void sparseScreenshotOcr_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<double>("scale");
        QTest::newRow("dark-100") << true << 1.;
        QTest::newRow("light-100") << false << 1.;
        QTest::newRow("dark-150") << true << 1.5;
        QTest::newRow("dark-200") << true << 2.;
    }
    void sparseScreenshotOcr() {
        QFETCH(bool, dark);
        QFETCH(double, scale);
        const auto image = desktopFixture(dark, scale);
        TesseractOcr engine;
        QSignalSpy done(&engine, &IOcrEngine::completed), failed(&engine, &IOcrEngine::failed);
        engine.recognize(image, findTessdata(), "chi_sim+eng", true);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 50000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        const auto lines = qvariant_cast<QVector<OcrLine>>(done.first().first());
        TextLayout text;
        text.setLines(lines);
        QVERIFY2(text.all().contains("PyCharm"), qPrintable(text.all()));
        QVERIFY2(text.all().contains("最近"), qPrintable(text.all()));
        int paths = 0;
        QSet<QString> ids;
        for (const auto &line : lines) {
            QVERIFY(!ids.contains(line.id));
            ids.insert(line.id);
            QVERIFY(QRectF(QPointF(), image.size()).contains(line.box));
            // 左菜单和右侧背景不能再合成横跨弹窗边缘的一行。
            QVERIFY2(!(line.box.left() < 800 * scale && line.box.right() > 852 * scale),
                     qPrintable(line.text + " " + QString::number(line.box.width())));
            if (line.text.contains("/Projects/")) {
                ++paths;
                QVERIFY(qAbs(line.box.left() / scale - 120) < 5);
                QVERIFY(line.box.height() / scale < 22);
            }
        }
        QVERIFY2(paths >= 6, qPrintable(text.all()));
    }
    void automaticCandidatesPreserveOriginals() {
        QVector<OcrLine> lines;
        auto add = [&](QString id, QString text, QRectF box, double confidence = .95) {
            OcrLine line;
            line.id = id; line.text = text; line.box = box; line.confidence = confidence;
            lines << line;
        };
        add("english", "Open project", {20, 10, 140, 18});
        add("chinese", "最近的项目", {20, 50, 110, 18});
        add("project", "minimind", {20, 100, 90, 18});
        add("path", "~/Projects/minimind", {20, 125, 180, 14});
        add("windows", "D:\\Projects\\SnapLing", {20, 160, 180, 14});
        add("identifier", "kitchen_safety", {20, 190, 140, 18});
        add("filename", "CMakeLists.txt", {20, 220, 140, 18});
        add("noise", "BU RR HE", {20, 250, 140, 18}, .2);
        add("low-confidence-valid", "Translate this label", {20, 270, 160, 18}, .3);
        add("log", "capture_ms=80 trigger_to_pixels_ms=10336", {20, 290, 320, 18});
        const auto automatic = translationUnits(lines, "简体中文", true);
        QCOMPARE(automatic.size(), 2);
        QCOMPARE(automatic.first().id, QString("english"));
        QCOMPARE(automatic.first().fontSize, 18);
        QCOMPARE(automatic.at(1).id, QString("low-confidence-valid"));
        for (const auto &unit : automatic)
            QVERIFY(unit.id != "noise");
        const auto explicitSelection = translationUnits({lines.at(7)}, "简体中文");
        QCOMPARE(explicitSelection.size(), 1);
        const auto english = translationUnits({lines.first(), lines.at(1)}, "English", true);
        QCOMPARE(english.size(), 1);
        QCOMPARE(english.first().id, QString("chinese"));
    }
    void noEligibleTextDoesNotCallProvider() {
        QTemporaryDir files;
        Settings settings(files.filePath("settings.ini"));
        ProviderConfig config;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        QSignalSpy requests(&server, &QTcpServer::newConnection);
        const auto image = desktopFixture(true);
        EditorSession session(image, settings, config);
        session.document.change([](auto &state) {
            OcrLine line;
            line.id = "chinese"; line.text = "最近的项目";
            line.box = {20, 20, 120, 18}; line.confidence = .95;
            state.ocr << line;
        });
        session.toggleTranslation();
        QVERIFY(!session.isTranslationEnabled());
        QCOMPARE(session.task(), EditorSession::Task::Idle);
        QCOMPARE(session.document.state().translationMode, 0);
        QCOMPARE(session.document.render(), image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        QTest::qWait(50);
        QVERIFY(requests.isEmpty());
    }
    void boundedRendering_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("panel");
        QTest::newRow("dark-auto") << true << false;
        QTest::newRow("dark-panel") << true << true;
        QTest::newRow("light-auto") << false << false;
        QTest::newRow("light-panel") << false << true;
    }
    void boundedRendering() {
        QFETCH(bool, dark);
        QFETCH(bool, panel);
        QImage image(400, 140, QImage::Format_RGB32);
        image.fill(dark ? QColor("#25272b") : QColor("#f5f6f8"));
        {
            QPainter p(&image);
            p.setPen(dark ? Qt::white : Qt::black);
            QFont font("Noto Sans CJK SC"); font.setPixelSize(16); p.setFont(font);
            p.drawText(20, 40, "Open project");
            p.drawText(230, 40, "邻列文字");
            p.drawText(20, 90, "minimind");
        }
        Document document(image);
        const QRectF box(20, 25, 120, 18);
        document.change([&](auto &state) {
            state.panel = panel;
            state.translationMode = 1;
            state.translations = {{"a", "Open project", "打开项目", "中文", "done", box, 16},
                                  {"b", "minimind", "minimind", "中文", "done", {20, 74, 100, 20}, 16}};
        });
        const auto fit = fitTranslation("打开项目", box, image.size(), 16);
        QVERIFY(!fit.overflow);
        QCOMPARE(fit.box, box);
        QVERIFY(fit.font.pixelSize() >= 14);
        const auto rendered = document.render();
        int changed = 0, textPixels = 0;
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x) {
                const auto pixel = rendered.pixelColor(x, y);
                if (!box.contains(QPointF(x, y)))
                    QCOMPARE(pixel, image.pixelColor(x, y));
                else {
                    changed += pixel != image.pixelColor(x, y);
                    textPixels += dark ? qGray(pixel.rgb()) > 200 : qGray(pixel.rgb()) < 60;
                }
            }
        QVERIFY(changed > 50);
        QVERIFY(textPixels > 50);
        const auto overflow = fitTranslation(QString(300, QChar(0x4e2d)), box, image.size(), 16);
        QVERIFY(overflow.overflow);
        QCOMPARE(overflow.box, box);
        document.change([&](auto &state) { state.translations[0].target = QString(300, QChar(0x4e2d)); });
        const auto kept = document.render();
        QCOMPARE(kept.pixelColor(80, 35), image.pixelColor(80, 35));
        QTemporaryDir files;
        QString error;
        QVERIFY(!document.exportImage(files.filePath("overflow.png"), error));
    }
    void darkMenuTranslationAndPng() {
        QTemporaryDir files;
        Settings settings(files.filePath("settings.ini"));
        settings.set("ocr/path", findTessdata());
        settings.set("ocr/languages", "chi_sim+eng");
        ProviderConfig config;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        config.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        QJsonArray sent;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", bytes);
                const auto boundary = bytes.indexOf("\r\n\r\n");
                QRegularExpression length("Content-Length: ([0-9]+)", QRegularExpression::CaseInsensitiveOption);
                const auto match = length.match(QString::fromLatin1(bytes.left(boundary)));
                if (boundary < 0 || !match.hasMatch() || bytes.size() < boundary + 4 + match.captured(1).toInt() ||
                    socket->property("answered").toBool()) return;
                socket->setProperty("answered", true);
                const auto messages = QJsonDocument::fromJson(bytes.mid(boundary + 4)).object()["messages"].toArray();
                sent = QJsonDocument::fromJson(messages.last().toObject()["content"].toString().toUtf8()).object()["blocks"].toArray();
                QJsonArray translations;
                for (const auto &block : sent)
                    translations << QJsonObject{{"id", block.toObject()["id"]}, {"text", "译文"}};
                const auto text = QString::fromUtf8(QJsonDocument(QJsonObject{{"translations", translations}}).toJson(QJsonDocument::Compact));
                const auto body = QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{
                    {"message", QJsonObject{{"content", text}}}, {"finish_reason", "stop"}}}}}).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        const auto image = desktopFixture(true);
        EditorSession session(image, settings, config);
        session.toggleTranslation();
        QTRY_VERIFY_WITH_TIMEOUT(!session.document.state().translations.isEmpty() ||
                                 (session.task() == EditorSession::Task::Idle && !session.isTranslationEnabled()), 15000);
        QVERIFY2(!session.document.state().translations.isEmpty(), qPrintable(session.message()));
        QVERIFY(!sent.isEmpty());
        for (const auto &block : sent) {
            QVERIFY(!block.toObject()["text"].toString().contains("/Projects/"));
            QVERIFY(!block.toObject()["text"].toString().contains("最近"));
            QVERIFY(block.toObject()["text"].toString().compare("study", Qt::CaseInsensitive) != 0);
            QVERIFY(block.toObject()["text"].toString().compare("SnapLing", Qt::CaseInsensitive) != 0);
        }
        QRegion changedRegions;
        for (const auto &unit : session.document.state().translations) {
            QVERIFY(!unit.overflow);
            QVERIFY(unit.box.width() < 500);
            const auto fit = fitTranslation(unit.target, unit.box, image.size(), unit.fontSize);
            QVERIFY(fit.font.pixelSize() >= qRound(unit.fontSize * .85));
            QCOMPARE(fit.box, unit.box);
            changedRegions += unit.box.toAlignedRect();
        }
        const auto rendered = session.document.render();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (!changedRegions.contains(QPoint(x, y)))
                    QCOMPARE(rendered.pixelColor(x, y), image.pixelColor(x, y));
        const auto output = files.filePath("translated.png");
        QString error;
        QVERIFY2(session.document.exportImage(output, error), qPrintable(error));
        QCOMPARE(QImage(output).convertToFormat(QImage::Format_ARGB32_Premultiplied), rendered);
        QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/translation-menu-original.png"));
        QVERIFY(rendered.save(QCoreApplication::applicationDirPath() + "/translation-menu-fixed.png"));
        session.toggleTranslation();
        QCOMPARE(session.document.render(), image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        session.toggleTranslation();
        QCOMPARE(session.document.render(), rendered);
    }
};
int main(int argc, char **argv) {
    qputenv("SNAPLING_OCR_TEST_ENGINE", "tesseract");
    QApplication app(argc, argv);
    TestTranslationScreenshot test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestTranslationScreenshot.moc"

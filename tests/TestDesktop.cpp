#include "capture/CaptureService.h"
#include "editor/Document.h"
#include "hotkey/Hotkey.h"
#include "selection/SelectionOverlay.h"
#include "ui/EditorWindow.h"
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#ifndef Q_OS_WIN
#include "platform/linux/x11/X11Capture.h"
#include <X11/Xlib.h>
#undef None
#endif
using namespace SnapLing;
class DesktopTarget : public QWidget {
  public:
    DesktopTarget() {
        setWindowTitle("SnapLing real desktop target");
        resize(800, 520);
        move(40, 40);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#f0f4fa"));
        p.fillRect(30, 30, 250, 180, QColor("#20c060"));
        p.setPen(Qt::black);
        QFont f("Noto Sans CJK SC");
        f.setPixelSize(36);
        p.setFont(f);
        p.drawText(30, 300, "SnapLing desktop capture");
        p.drawText(30, 370, "真实桌面截图 中文识别");
    }
};
class TestDesktop : public QObject {
    Q_OBJECT
  private slots:
    void capturePopupEditSave() {
#ifdef Q_OS_WIN
        QSKIP("Windows 桌面测试需要交互式运行器");
#else
        QVERIFY(QGuiApplication::platformName() == "xcb");
        QDir().mkpath("test-artifacts");
        DesktopTarget target;
        target.show();
        QTest::qWait(150);
        QMenu popup;
        popup.addAction("Popup must remain visible");
        popup.addAction("真实弹窗保留测试");
        popup.popup(target.mapToGlobal(QPoint(340, 60)));
        QTest::qWait(120);
        QVERIFY(popup.isVisible());
        X11Capture backend;
        QSignalSpy captured(&backend, &ICaptureBackend::captured), errors(&backend, &ICaptureBackend::failed);
        qRegisterMetaType<CaptureFrame>();
        backend.capture(CaptureMode::Region);
        QTRY_VERIFY_WITH_TIMEOUT(captured.size() == 1 || !errors.isEmpty(), 5000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors[0][0].toString()));
        QVERIFY(popup.isVisible());
        auto frame = qvariant_cast<CaptureFrame>(captured[0][0]);
        QVERIFY(!frame.image.isNull());
        auto pixel = frame.toPixel(target.mapToGlobal(QPoint(100, 100))).toPoint();
        QCOMPARE(frame.image.pixelColor(pixel), QColor("#20c060"));
        auto popupRect = frame.pixelRect(popup.geometry());
        QVERIFY(!popupRect.isEmpty());
        auto capturedPopup = frame.image.copy(popupRect);
        auto popupGrab = popup.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(capturedPopup.size(), popupGrab.size());
        QCOMPARE(capturedPopup.convertToFormat(QImage::Format_RGB32), popupGrab);
        QVERIFY(frame.image.save("test-artifacts/desktop-with-popup.png"));
        popup.hide();
        auto *overlay = new SelectionOverlay(frame, CaptureMode::Region);
        QTest::qWait(80);
        auto from = overlay->mapFromGlobal(target.mapToGlobal(QPoint(15, 15)));
        auto to = overlay->mapFromGlobal(target.mapToGlobal(QPoint(780, 490)));
        QSignalSpy selected(overlay, &SelectionOverlay::accepted);
        QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(overlay, to, 40);
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, to);
        QTest::keyClick(overlay, Qt::Key_Return);
        QCOMPARE(selected.size(), 1);
        auto image = qvariant_cast<QImage>(selected[0][0]);
        QCOMPARE(image.size(), QSize(765, 475));
        Document doc(image);
        Annotation arrow;
        arrow.tool = Tool::Arrow;
        arrow.points << QPointF(30, 230) << QPointF(250, 250);
        doc.change([arrow](auto &s) { s.annotations << arrow; });
        QString error;
        QVERIFY2(doc.exportImage("test-artifacts/desktop-edited.png", error), qPrintable(error));
        QVERIFY(doc.exportImage("test-artifacts/desktop-edited.jpg", error));
        QVERIFY(doc.saveProject("test-artifacts/desktop.snapling", error));
        Document restored;
        QVERIFY(restored.loadProject("test-artifacts/desktop.snapling", error));
        QCOMPARE(restored.render(), doc.render());
        QApplication::clipboard()->setImage(doc.render());
        QCOMPARE(QApplication::clipboard()->image(), doc.render());
        QTemporaryDir temp;
        Settings settings(temp.filePath("settings.ini"));
        ProviderConfig config;
        EditorWindow editor(image, settings, config);
        editor.document()->change([arrow](auto &s) { s.annotations << arrow; });
        editor.show();
        QTest::qWait(100);
        TesseractOcr ocr;
        QSignalSpy recognized(&ocr, &IOcrEngine::completed), ocrErrors(&ocr, &IOcrEngine::failed);
        ocr.recognize(frame.image, findTessdata(), "chi_sim+eng", true);
        QTRY_VERIFY_WITH_TIMEOUT(!recognized.isEmpty() || !ocrErrors.isEmpty(), 45000);
        QVERIFY2(ocrErrors.isEmpty(), ocrErrors.isEmpty() ? "" : qPrintable(ocrErrors[0][0].toString()));
        qInfo("desktop_1080p_ocr_cold_ms=%lld", recognized[0][1].toLongLong());
        ocr.recognize(frame.image, findTessdata(), "chi_sim+eng", true);
        QTRY_COMPARE_WITH_TIMEOUT(recognized.size(), 2, 45000);
        QVERIFY(!recognized[1][2].toBool());
        qInfo("desktop_1080p_ocr_warm_ms=%lld", recognized[1][1].toLongLong());
        auto cropOrigin = frame.toPixel(target.mapToGlobal(QPoint(15, 15)));
        auto lines = mapOcr(qvariant_cast<QVector<OcrLine>>(recognized[0][0]),
                            QTransform::fromTranslate(-cropOrigin.x(), -cropOrigin.y()));
        editor.document()->change([lines](auto &s) { s.ocr = lines; });
        auto *canvas = editor.canvasWidget();
        canvas->setTool(Tool::Ocr);
        TextLayout text;
        text.setLines(lines);
        int lineIndex = -1;
        for (int i = 0; i < text.lines().size(); ++i)
            if (text.lines()[i].text.contains("中文")) {
                lineIndex = i;
                break;
            }
        QVERIFY2(lineIndex >= 0, qPrintable(text.all()));
        auto charBoxes = text.characterBoxes(lineIndex);
        auto transform = canvas->sourceToWidget();
        auto start = transform.map(charBoxes.first().center()).toPoint(),
             end = transform.map(charBoxes.last().center()).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, end, 20);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(canvas->selectedText(), text.lines()[lineIndex].text);
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), text.lines()[lineIndex].text);
        QVERIFY(editor.grab().save("test-artifacts/editor-ui.png"));
        editor.hide();
#endif
    }
    void actualGlobalHotkey() {
#ifndef Q_OS_WIN
        auto backend = createHotkeyBackend();
        QSignalSpy activated(backend.get(), &IHotkeyBackend::activated),
            status(backend.get(), &IHotkeyBackend::status);
        backend->bind(QKeySequence("Ctrl+Alt+A"));
        QVERIFY(!status.isEmpty());
        QVERIFY(status.last()[1].toBool());
        QCOMPARE(QProcess::execute("xdotool", {"key", "ctrl+alt+a"}), 0);
        QTRY_COMPARE_WITH_TIMEOUT(activated.size(), 1, 3000);
        auto other = createHotkeyBackend();
        QSignalSpy collision(other.get(), &IHotkeyBackend::status);
        other->bind(QKeySequence("Ctrl+Alt+A"));
        QVERIFY(!collision.last()[1].toBool());
        backend->unbind();
        other->bind(QKeySequence("Ctrl+Alt+A"));
        QVERIFY(collision.last()[1].toBool());
#endif
    }
    void repeatCaptureBoundedMemory() {
#ifndef Q_OS_WIN
        auto rss = []() {
            QFile f("/proc/self/status");
            if (!f.open(QIODevice::ReadOnly))
                return qint64(0);
            auto data = QString::fromLatin1(f.readAll());
            QRegularExpression re("VmRSS:\\s+(\\d+)");
            return re.match(data).captured(1).toLongLong();
        };
        X11Capture backend;
        QSignalSpy frames(&backend, &ICaptureBackend::captured);
        QVector<qint64> times, paintTimes;
        QElapsedTimer trigger;
        QPointer<SelectionOverlay> overlay;
        connect(&backend, &ICaptureBackend::captured, this, [&](CaptureFrame frame) {
            overlay = new SelectionOverlay(frame, CaptureMode::Region);
            connect(overlay, &SelectionOverlay::ready, this, [&] { paintTimes << trigger.elapsed(); });
        });
        qint64 before = rss();
        for (int i = 0; i < 25; ++i) {
            trigger.start();
            backend.capture(CaptureMode::FullScreen);
            QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 1, 5000);
            auto frame = qvariant_cast<CaptureFrame>(frames.takeFirst()[0]);
            times << frame.captureMs;
            QTRY_COMPARE_WITH_TIMEOUT(paintTimes.size(), i + 1, 5000);
            if (overlay)
                overlay->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QVERIFY2(overlay.isNull(), "选区窗口未释放");
        }
        qint64 after = rss();
        std::sort(times.begin(), times.end());
        std::sort(paintTimes.begin(), paintTimes.end());
        qInfo("x11_capture_to_selection_p95_ms=%lld", paintTimes[23]);
        qInfo("x11_capture_p95_ms=%lld rss_before_kib=%lld rss_after_kib=%lld", times[23], before, after);
        QVERIFY2(after - before < 48 * 1024, "连续截图内存增长超过 48 MiB");
#endif
    }
};
int main(int argc, char **argv) {
#ifndef Q_OS_WIN
    XInitThreads();
    if (!qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION"))
        qputenv("QT_XCB_GL_INTEGRATION", "none");
#endif
#ifdef SNAPLING_TEST_MODEL_DIR
    if (!qEnvironmentVariableIsSet("TESSDATA_PREFIX"))
        qputenv("TESSDATA_PREFIX", SNAPLING_TEST_MODEL_DIR);
#endif
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
    app.setApplicationName("SnapLingDesktopTest");
    if (app.arguments().contains("--target-only")) {
        DesktopTarget target;
        target.show();
        QMenu menu;
        menu.addAction("Popup must remain visible");
        menu.addAction("真实弹窗保留测试");
        QTimer::singleShot(100, &target, [&] { menu.popup(target.mapToGlobal(QPoint(340, 60))); });
        QTimer::singleShot(20000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    TestDesktop test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestDesktop.moc"

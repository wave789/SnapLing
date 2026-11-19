#include "WindowsCapture.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QtConcurrent>
#include <dwmapi.h>
#include <windows.h>
namespace SnapLing {
namespace {
struct DC {
    HDC value = nullptr;
    HWND owner = nullptr;
    ~DC() {
        if (value)
            ReleaseDC(owner, value);
    }
};
struct MemDC {
    HDC value = nullptr;
    ~MemDC() {
        if (value)
            DeleteDC(value);
    }
};
struct Bitmap {
    HBITMAP value = nullptr;
    ~Bitmap() {
        if (value)
            DeleteObject(value);
    }
};
struct Selection {
    HDC dc;
    HGDIOBJ previous;
    ~Selection() {
        SelectObject(dc, previous);
    }
};
struct Result {
    CaptureFrame frame;
    QString error;
};
} // namespace
void WindowsCapture::capture(CaptureMode, int screen) {
    cancel();
    token = std::make_shared<CancelToken>();
    auto t = token;
    // Qt 与 Win32 坐标在混合 DPI 上不同。每个显示器分别采集，再映射至统一逻辑桌面。
    struct Monitor {
        QRect logical;
        QRect native;
    };
    QVector<Monitor> monitors;
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        if (screen >= 0 && i != screen)
            continue;
        auto *s = screens[i];
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(reinterpret_cast<LPCWSTR>(s->name().utf16()), ENUM_CURRENT_SETTINGS, &dm))
            monitors.append({s->geometry(), QRect(dm.dmPosition.x, dm.dmPosition.y, int(dm.dmPelsWidth),
                                                  int(dm.dmPelsHeight))});
    }
    auto *w = new QFutureWatcher<Result>(this);
    connect(w, &QFutureWatcher<Result>::finished, this, [this, w, t] {
        auto r = w->result();
        w->deleteLater();
        if (t->isCancelled())
            return;
        if (r.error.isEmpty())
            emit captured(r.frame);
        else
            emit failed(r.error);
    });
    w->setFuture(QtConcurrent::run(&workerPool, [monitors, t] {
        Result r;
        QElapsedTimer timer;
        timer.start();
        QRect logical;
        qreal scale = 1;
        for (auto m : monitors) {
            logical = logical.united(m.logical);
            scale = qMax(scale, qreal(m.native.width()) / m.logical.width());
        }
        if (logical.isEmpty()) {
            r.error = "找不到显示器";
            return r;
        }
        if (qint64(logical.width() * scale) * qint64(logical.height() * scale) > 100000000) {
            r.error = "桌面超过 1 亿像素限制";
            return r;
        }
        r.frame.desktop = logical;
        r.frame.image = QImage(QSize(qRound(logical.width() * scale), qRound(logical.height() * scale)),
                               QImage::Format_RGB32);
        if (r.frame.image.isNull()) {
            r.error = "虚拟桌面内存不足";
            return r;
        }
        r.frame.image.fill(Qt::black);
        QPainter painter(&r.frame.image);
        DC screenDC{GetDC(nullptr)};
        if (!screenDC.value) {
            r.error = "无法获取桌面 DC";
            return r;
        }
        for (auto m : monitors) {
            if (t->isCancelled())
                return r;
            MemDC mem{CreateCompatibleDC(screenDC.value)};
            Bitmap bm{CreateCompatibleBitmap(screenDC.value, m.native.width(), m.native.height())};
            if (!mem.value || !bm.value) {
                r.error = "GDI 资源申请失败";
                return r;
            }
            Selection selection{mem.value, SelectObject(mem.value, bm.value)};
            if (!BitBlt(mem.value, 0, 0, m.native.width(), m.native.height(), screenDC.value, m.native.x(),
                        m.native.y(), SRCCOPY | CAPTUREBLT)) {
                r.error = "桌面捕获被系统拒绝";
                return r;
            }
            QImage image(m.native.size(), QImage::Format_RGB32);
            if (image.isNull()) {
                r.error = "显示器图像内存不足";
                return r;
            }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = image.width();
            info.bmiHeader.biHeight = -image.height();
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SelectObject(mem.value, selection.previous);
            if (!GetDIBits(mem.value, bm.value, 0, UINT(image.height()), image.bits(), &info,
                           DIB_RGB_COLORS)) {
                r.error = "读取屏幕像素失败";
                return r;
            }
            // GDI 的 32 位 DIB 最高字节未定义，进入 Qt 像素管线前明确设为不透明。
            for (int y = 0; y < image.height(); ++y) {
                auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x)
                    row[x] |= 0xff000000u;
            }
            painter.drawImage(QRectF((m.logical.x() - logical.x()) * scale,
                                     (m.logical.y() - logical.y()) * scale, m.logical.width() * scale,
                                     m.logical.height() * scale),
                              image);
        }
        std::pair<Result *, const QVector<Monitor> *> context{&r, &monitors};
        EnumWindows(
            [](HWND hwnd, LPARAM data) -> BOOL {
                auto *pair = reinterpret_cast<std::pair<Result *, const QVector<Monitor> *> *>(data);
                if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
                    return TRUE;
                RECT rect{};
                if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
                    GetWindowRect(hwnd, &rect);
                for (auto m : *pair->second)
                    if (m.native.contains(QPoint(rect.left, rect.top))) {
                        qreal sx = qreal(m.logical.width()) / m.native.width(),
                              sy = qreal(m.logical.height()) / m.native.height();
                        pair->first->frame.windows.append(
                            {QRectF(m.logical.x() + (rect.left - m.native.x()) * sx,
                                    m.logical.y() + (rect.top - m.native.y()) * sy,
                                    (rect.right - rect.left) * sx, (rect.bottom - rect.top) * sy),
                             {},
                             quintptr(hwnd)});
                        break;
                    }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
        painter.end();
        r.frame.captureMs = timer.elapsed();
        return r;
    }));
}
} // namespace SnapLing

#include "X11Capture.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QtConcurrent>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <cstring>
#include <xcb/xcb.h>
namespace SnapLing {
namespace {
struct Result {
    CaptureFrame frame;
    QString error;
};
struct Monitor {
    QString name;
    QRect logical, native;
};
template <class T> using CPtr = std::unique_ptr<T, decltype(&std::free)>;
unsigned component(quint32 pixel, quint32 mask) {
    if (!mask)
        return 0;
    unsigned shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++shift;
    }
    return ((pixel >> shift) & mask) * 255 / mask;
}
} // namespace
void X11Capture::capture(CaptureMode, int screen) {
    cancel();
    token = std::make_shared<CancelToken>();
    auto t = token;
    QVector<Monitor> monitors;
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        if (screen >= 0 && i != screen)
            continue;
        auto *s = screens[i];
        QRect l = s->geometry();
        monitors.append(
            {s->name(), l,
             QRect(qRound(l.x() * s->devicePixelRatio()), qRound(l.y() * s->devicePixelRatio()),
                   qRound(l.width() * s->devicePixelRatio()), qRound(l.height() * s->devicePixelRatio()))});
    }
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, t] {
        auto r = watcher->result();
        watcher->deleteLater();
        if (t->isCancelled())
            return;
        if (r.error.isEmpty())
            emit captured(r.frame);
        else
            emit failed(r.error);
    });
    watcher->setFuture(QtConcurrent::run(&workerPool, [monitors = std::move(monitors), t]() mutable {
        Result r;
        QElapsedTimer timer;
        timer.start();
        // 物理显示器位置由 RandR 获取，不把混合 DPI 下的逻辑原点直接乘全局倍率。
        std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
        if (!display) {
            r.error = "无法连接 X11 显示服务器";
            return r;
        }
        auto root = DefaultRootWindow(display.get());
        std::unique_ptr<XRRScreenResources, decltype(&XRRFreeScreenResources)> resources(
            XRRGetScreenResourcesCurrent(display.get(), root), XRRFreeScreenResources);
        if (resources)
            for (int i = 0; i < resources->noutput; ++i) {
                std::unique_ptr<XRROutputInfo, decltype(&XRRFreeOutputInfo)> out(
                    XRRGetOutputInfo(display.get(), resources.get(), resources->outputs[i]),
                    XRRFreeOutputInfo);
                if (!out || out->connection != RR_Connected || !out->crtc)
                    continue;
                std::unique_ptr<XRRCrtcInfo, decltype(&XRRFreeCrtcInfo)> crtc(
                    XRRGetCrtcInfo(display.get(), resources.get(), out->crtc), XRRFreeCrtcInfo);
                if (!crtc)
                    continue;
                QString name = QString::fromLatin1(out->name, out->nameLen);
                for (auto &m : monitors)
                    if (m.name == name)
                        m.native = QRect(crtc->x, crtc->y, int(crtc->width), int(crtc->height));
            }
        int screenNumber = 0;
        std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> conn(xcb_connect(nullptr, &screenNumber),
                                                                          xcb_disconnect);
        if (xcb_connection_has_error(conn.get())) {
            r.error = "XCB 桌面连接失败";
            return r;
        }
        const auto *setup = xcb_get_setup(conn.get());
        auto it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < screenNumber && it.rem; ++i)
            xcb_screen_next(&it);
        if (!it.rem) {
            r.error = "X11 根屏幕不存在";
            return r;
        }
        auto *nativeScreen = it.data;
        QRect nativeBounds;
        QRect logicalBounds;
        qreal scale = 1;
        for (auto m : monitors) {
            nativeBounds = nativeBounds.united(m.native);
            logicalBounds = logicalBounds.united(m.logical);
            scale = qMax(scale, qreal(m.native.width()) / qMax(1, m.logical.width()));
        }
        nativeBounds = nativeBounds.intersected(
            QRect(0, 0, nativeScreen->width_in_pixels, nativeScreen->height_in_pixels));
        if (nativeBounds.isEmpty() || logicalBounds.isEmpty() ||
            qint64(logicalBounds.width() * scale) * qint64(logicalBounds.height() * scale) > 100000000) {
            r.error = "显示器几何无效或桌面超过 1 亿像素限制";
            return r;
        }
        CPtr<xcb_generic_error_t> error(nullptr, std::free);
        xcb_generic_error_t *err = nullptr;
        CPtr<xcb_get_image_reply_t> reply(
            xcb_get_image_reply(conn.get(),
                                xcb_get_image(conn.get(), XCB_IMAGE_FORMAT_Z_PIXMAP, nativeScreen->root,
                                              int16_t(nativeBounds.x()), int16_t(nativeBounds.y()),
                                              uint16_t(nativeBounds.width()), uint16_t(nativeBounds.height()),
                                              ~0u),
                                &err),
            std::free);
        error.reset(err);
        if (!reply || error) {
            r.error = "X11 桌面像素捕获失败";
            return r;
        }
        quint32 red = 0xff0000, green = 0xff00, blue = 0xff;
        for (auto depth = xcb_screen_allowed_depths_iterator(nativeScreen); depth.rem; xcb_depth_next(&depth))
            for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem;
                 xcb_visualtype_next(&visual))
                if (visual.data->visual_id == reply->visual) {
                    red = visual.data->red_mask;
                    green = visual.data->green_mask;
                    blue = visual.data->blue_mask;
                }
        int bits = 0, pad = 32;
        for (auto format = xcb_setup_pixmap_formats_iterator(setup); format.rem; xcb_format_next(&format))
            if (format.data->depth == reply->depth) {
                bits = format.data->bits_per_pixel;
                pad = format.data->scanline_pad;
                break;
            }
        if (bits != 32 && bits != 24) {
            r.error = "当前 X11 像素格式不受支持（需要 24/32 位）";
            return r;
        }
        int stride = ((nativeBounds.width() * bits + pad - 1) / pad) * (pad / 8);
        auto *data = xcb_get_image_data(reply.get());
        if (qint64(stride) * nativeBounds.height() > xcb_get_image_data_length(reply.get())) {
            r.error = "X11 返回的图像数据不完整";
            return r;
        }
        QImage native(nativeBounds.size(), QImage::Format_RGB32);
        if (native.isNull()) {
            r.error = "截图内存不足";
            return r;
        }
        for (int y = 0; y < native.height(); ++y) {
            if (t->isCancelled())
                return r;
            auto *dst = reinterpret_cast<QRgb *>(native.scanLine(y));
            for (int x = 0; x < native.width(); ++x) {
                quint32 pixel = 0;
                auto *src = data + y * stride + x * (bits / 8);
                if (setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST)
                    for (int b = 0; b < bits / 8; ++b)
                        pixel |= quint32(src[b]) << (b * 8);
                else
                    for (int b = 0; b < bits / 8; ++b)
                        pixel = (pixel << 8) | src[b];
                dst[x] = qRgb(component(pixel, red), component(pixel, green), component(pixel, blue));
            }
        }
        r.frame.desktop = logicalBounds;
        r.frame.image =
            QImage(QSize(qRound(logicalBounds.width() * scale), qRound(logicalBounds.height() * scale)),
                   QImage::Format_RGB32);
        if (r.frame.image.isNull()) {
            r.error = "虚拟桌面内存不足";
            return r;
        }
        r.frame.image.fill(Qt::black);
        QPainter p(&r.frame.image);
        for (auto m : monitors) {
            auto target = QRectF((m.logical.x() - logicalBounds.x()) * scale,
                                 (m.logical.y() - logicalBounds.y()) * scale, m.logical.width() * scale,
                                 m.logical.height() * scale);
            p.drawImage(target, native, m.native.translated(-nativeBounds.topLeft()));
        }
        p.end();
        // XCB 对已消失窗口返回错误，不会触发 Xlib 默认致命错误处理。
        CPtr<xcb_query_tree_reply_t> tree(
            xcb_query_tree_reply(conn.get(), xcb_query_tree(conn.get(), nativeScreen->root), nullptr),
            std::free);
        if (tree) {
            auto *children = xcb_query_tree_children(tree.get());
            for (int i = xcb_query_tree_children_length(tree.get()) - 1; i >= 0; --i) {
                CPtr<xcb_get_window_attributes_reply_t> attrs(
                    xcb_get_window_attributes_reply(
                        conn.get(), xcb_get_window_attributes(conn.get(), children[i]), nullptr),
                    std::free);
                if (!attrs || attrs->map_state != XCB_MAP_STATE_VIEWABLE)
                    continue;
                CPtr<xcb_get_geometry_reply_t> g(
                    xcb_get_geometry_reply(conn.get(), xcb_get_geometry(conn.get(), children[i]), nullptr),
                    std::free);
                if (!g || g->width < 2 || g->height < 2)
                    continue;
                CPtr<xcb_translate_coordinates_reply_t> pos(
                    xcb_translate_coordinates_reply(
                        conn.get(),
                        xcb_translate_coordinates(conn.get(), children[i], nativeScreen->root, 0, 0),
                        nullptr),
                    std::free);
                if (!pos)
                    continue;
                for (auto m : monitors)
                    if (m.native.contains(pos->dst_x, pos->dst_y)) {
                        qreal sx = qreal(m.logical.width()) / m.native.width(),
                              sy = qreal(m.logical.height()) / m.native.height();
                        r.frame.windows.append({QRectF(m.logical.x() + (pos->dst_x - m.native.x()) * sx,
                                                       m.logical.y() + (pos->dst_y - m.native.y()) * sy,
                                                       g->width * sx, g->height * sy),
                                                {},
                                                children[i]});
                        break;
                    }
            }
        }
        r.frame.captureMs = timer.elapsed();
        return r;
    }));
}
} // namespace SnapLing

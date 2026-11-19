#include "PipeWireStream.h"
#include <QDateTime>
#include <QMutexLocker>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <unistd.h>
namespace SnapLing {
struct PipeWireStream::Impl {
    PipeWireStream *owner;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw info{};
    mutable QMutex mutex;
    QImage frame;
    qint64 timestamp = 0;
    static void format(void *p, uint32_t id, const spa_pod *param) {
        auto *d = static_cast<Impl *>(p);
        if (param && id == SPA_PARAM_Format)
            spa_format_video_raw_parse(param, &d->info);
    }
    static void state(void *p, pw_stream_state, pw_stream_state state, const char *error) {
        auto *d = static_cast<Impl *>(p);
        if (state == PW_STREAM_STATE_ERROR) {
            QString e = QString::fromUtf8(error ? error : "PipeWire 会话失败");
            QMetaObject::invokeMethod(
                d->owner, [o = d->owner, e] { emit o->failed(e); }, Qt::QueuedConnection);
        }
    }
    static void process(void *p) {
        auto *d = static_cast<Impl *>(p);
        pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
        if (!b)
            return;
        spa_buffer *buf = b->buffer;
        if (buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
            auto &v = buf->datas[0];
            int w = int(d->info.size.width), h = int(d->info.size.height), stride = v.chunk->stride;
            QImage::Format f = QImage::Format_Invalid;
            if (d->info.format == SPA_VIDEO_FORMAT_BGRx || d->info.format == SPA_VIDEO_FORMAT_BGRA)
                f = QImage::Format_RGB32;
            if (d->info.format == SPA_VIDEO_FORMAT_RGBx || d->info.format == SPA_VIDEO_FORMAT_RGBA)
                f = QImage::Format_RGBX8888;
            const quint64 offset = v.chunk->offset;
            if (f != QImage::Format_Invalid && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
                stride >= w * 4 && offset + quint64(stride) * quint64(h) <= v.maxsize) {
                QImage image(static_cast<const uchar *>(v.data) + offset, w, h, stride, f);
                QMutexLocker lock(&d->mutex);
                d->frame = image.copy();
                d->timestamp = QDateTime::currentMSecsSinceEpoch(); // 最多保留一帧，不发信号积压帧队列。
            }
        }
        pw_stream_queue_buffer(d->stream, b);
    }
};
PipeWireStream::PipeWireStream(QObject *p) : QObject(p), impl(std::make_unique<Impl>()) {
    impl->owner = this;
}
PipeWireStream::~PipeWireStream() {
    stop();
}
bool PipeWireStream::start(int fd, uint node, const QString &serial, QString &error) {
    stop();
    pw_init(nullptr, nullptr);
    auto &d = *impl;
    d.loop = pw_thread_loop_new("snapling", nullptr);
    if (!d.loop) {
        ::close(fd);
        error = "无法创建 PipeWire 线程";
        return false;
    }
    d.context = pw_context_new(pw_thread_loop_get_loop(d.loop), nullptr, 0);
    if (!d.context) {
        ::close(fd);
        error = "无法创建 PipeWire 上下文";
        stop();
        return false;
    }
    d.core = pw_context_connect_fd(d.context, fd, nullptr, 0); // fd 所有权交给 PipeWire。
    if (!d.core) {
        error = "PipeWire 远程连接失败";
        stop();
        return false;
    }
    QByteArray target = (serial.isEmpty() ? QString::number(node) : serial).toUtf8();
    d.stream = pw_stream_new(d.core, "SnapLing capture",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                               PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT,
                                               target.constData(), nullptr));
    if (!d.stream) {
        error = "无法创建 PipeWire 视频流";
        stop();
        return false;
    }
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = Impl::state;
        e.param_changed = Impl::format;
        e.process = Impl::process;
        return e;
    }();
    pw_stream_add_listener(d.stream, &d.listener, &events, &d);
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *param = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                               SPA_VIDEO_FORMAT_RGBA)));
    int result = pw_stream_connect(
        d.stream, PW_DIRECTION_INPUT, node,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), &param, 1);
    if (result < 0 || pw_thread_loop_start(d.loop) < 0) {
        error = "PipeWire 视频流启动失败";
        stop();
        return false;
    }
    return true;
}
void PipeWireStream::stop() {
    auto &d = *impl;
    if (d.loop)
        pw_thread_loop_stop(d.loop);
    if (d.stream) {
        spa_hook_remove(&d.listener);
        pw_stream_destroy(d.stream);
        d.stream = nullptr;
    }
    if (d.core) {
        pw_core_disconnect(d.core);
        d.core = nullptr;
    }
    if (d.context) {
        pw_context_destroy(d.context);
        d.context = nullptr;
    }
    if (d.loop) {
        pw_thread_loop_destroy(d.loop);
        d.loop = nullptr;
    }
    QMutexLocker lock(&d.mutex);
    d.frame = {};
    d.timestamp = 0;
}
QImage PipeWireStream::latest() const {
    QMutexLocker lock(&impl->mutex);
    if (QDateTime::currentMSecsSinceEpoch() - impl->timestamp > 100)
        return {};
    return impl->frame;
}
} // namespace SnapLing

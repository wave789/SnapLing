#include "WaylandCapture.h"
#include "Portal.h"
#ifdef SNAPLING_HAVE_PIPEWIRE
#include "PipeWireStream.h"
#endif
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>
#include <unistd.h>
namespace SnapLing {
static const QString iface = "org.freedesktop.portal.ScreenCast";
WaylandCapture::WaylandCapture(QObject *p, StreamFactory factory)
    : ICaptureBackend(p), streamFactory(std::move(factory)) {
    frameTimer.setInterval(20);
    connect(&frameTimer, &QTimer::timeout, this, &WaylandCapture::deliverFrame);
}
WaylandCapture::~WaylandCapture() {
    resetSharing();
}
QString WaylandCapture::capabilities() const {
    return QString("Screenshot v%1 · ScreenCast v%2 · GlobalShortcuts v%3\nWayland "
                   "首次授权屏幕访问后，由 SnapLing 框选并即时标注。普通截图取得一帧后结束共享。")
        .arg(PortalRequest::version("org.freedesktop.portal.Screenshot"))
        .arg(PortalRequest::version(iface))
        .arg(PortalRequest::version("org.freedesktop.portal.GlobalShortcuts"));
}
void WaylandCapture::cancel() {
    if (temporarySharing || starting || request)
        resetSharing();
    else {
        ++generation;
        capturePending = false;
        frameTimer.stop();
    }
}
void WaylandCapture::capture(CaptureMode, int) {
    if (capturePending) {
        emit failed("已有截图请求，请先完成或取消");
        return;
    }
    capturePending = true;
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (active && stream) {
        waitForFrame();
        return;
    }
    if (!starting)
        beginSharing(true);
#else
    ++generation;
    portalCapture();
#endif
}
void WaylandCapture::portalCapture() {
    const auto job = generation;
    request = new PortalRequest(this);
    request->call("org.freedesktop.portal.Screenshot", "Screenshot", {QString()},
                  {{"interactive", false}, {"modal", false}},
                  [this, job](uint code, QVariantMap data, QString error) {
        if (job != generation)
            return;
        request = nullptr;
        if (code) {
            failSharing(error.isEmpty()
                ? "系统未提供完整屏幕图像。即时框选需要启用 PipeWire，请安装开发依赖并重新构建。"
                : "系统截图失败：" + error + "。即时框选需要启用 PipeWire。");
            return;
        }
        const QUrl uri(data["uri"].toString());
        if (!uri.isLocalFile()) {
            failSharing("系统未返回有效的本地截图文件");
            return;
        }
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, job] {
            const auto image = watcher->result();
            watcher->deleteLater();
            if (job != generation)
                return;
            if (image.isNull()) {
                failSharing("系统截图文件无法读取");
                return;
            }
            CaptureFrame frame;
            frame.image = image;
            frame.desktop = QGuiApplication::primaryScreen()->geometry();
            capturePending = false;
            emit captured(frame);
        });
        watcher->setFuture(QtConcurrent::run(&workerPool, [path = uri.toLocalFile()] {
            QImage image(path);
            image.setDevicePixelRatio(1);
            return image;
        }));
    });
}
void WaylandCapture::startSharing() {
    if (!active && !starting)
        beginSharing(false);
}
void WaylandCapture::beginSharing(bool temporary) {
#ifndef SNAPLING_HAVE_PIPEWIRE
    Q_UNUSED(temporary)
    failSharing("此构建未启用 PipeWire，请安装 libpipewire-0.3-dev 并重新构建。");
#else
    if (PortalRequest::version(iface) == 0) {
        failSharing("系统未提供 ScreenCast Portal，无法取得即时框选所需的完整屏幕帧。");
        return;
    }
    starting = true;
    temporarySharing = temporary;
    const auto job = ++generation;
    request = new PortalRequest(this);
    request->call(iface, "CreateSession", {},
                  {{"session_handle_token", "snapling_" + QUuid::createUuid().toString(QUuid::Id128)}},
                  [this, job](uint code, QVariantMap data, QString error) {
        if (job != generation)
            return;
        request = nullptr;
        if (code) {
            failSharing(error.isEmpty() ? "屏幕访问授权已取消" : error);
            return;
        }
        const auto handle = data.value("session_handle");
        session = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                      ? qvariant_cast<QDBusObjectPath>(handle).path() : handle.toString();
        if (session.isEmpty()) {
            failSharing("Portal 未返回会话");
            return;
        }
        QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop", session,
            "org.freedesktop.portal.Session", "Closed", this, SLOT(sessionClosed()));
        selectSources();
    });
#endif
}
void WaylandCapture::waitForFrame() {
    frameWait.start();
    frameTimer.start();
    deliverFrame();
}
void WaylandCapture::deliverFrame() {
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (!capturePending || !active || !stream)
        return;
    const auto image = stream->latest();
    if (image.isNull()) {
        if (frameWait.elapsed() >= 10000)
            failSharing("10 秒内未获得可读取的屏幕帧，请检查 PipeWire 屏幕共享服务后重试。");
        return;
    }
    CaptureFrame frame;
    frame.image = image;
    frame.desktop = sharedDesktop;
    frame.captureMs = frameWait.elapsed();
    capturePending = false;
    frameTimer.stop();
    if (temporarySharing)
        resetSharing();
    emit captured(frame);
#endif
}
void WaylandCapture::selectSources() {
    QVariantMap opts{{"types", uint(1)}, {"multiple", false}, {"cursor_mode", uint(1)}};
    if (PortalRequest::version(iface) >= 4) {
        opts["persist_mode"] = uint(2);
        QString token = QSettings().value("wayland/restoreToken").toString();
        if (!token.isEmpty())
            opts["restore_token"] = token;
    }
    const auto job = generation;
    request = new PortalRequest(this);
    request->call(iface, "SelectSources", {QVariant::fromValue(QDBusObjectPath(session))}, opts,
                  [this, job](uint code, QVariantMap, QString error) {
                      if (job != generation)
                          return;
                      request = nullptr;
                      if (code) {
                          failSharing(error.isEmpty() ? "无法选择屏幕共享来源" : error);
                          return;
                      }
                      startSession();
                  });
}
void WaylandCapture::startSession() {
    const auto job = generation;
    request = new PortalRequest(this);
    request->call(iface, "Start", {QVariant::fromValue(QDBusObjectPath(session)), QString()}, {},
                  [this, job](uint code, QVariantMap data, QString error) {
                      if (job != generation)
                          return;
                      request = nullptr;
                      if (code) {
                          failSharing(error.isEmpty() ? "屏幕访问被拒绝" : error);
                          return;
                      }
                      openRemote(data);
                  });
}
void WaylandCapture::openRemote(const QVariantMap &data) {
#ifdef SNAPLING_HAVE_PIPEWIRE
    const auto arg = qvariant_cast<QDBusArgument>(data["streams"]);
    if (arg.currentSignature() != "a(ua{sv})") {
        failSharing("Portal 未返回有效的视频节点列表");
        return;
    }
    uint node = 0;
    QVariantMap props;
    arg.beginArray();
    if (!arg.atEnd()) {
        arg.beginStructure();
        arg >> node >> props;
        arg.endStructure();
    }
    arg.endArray();
    if (!node) {
        failSharing("Portal 未返回可用的视频节点");
        return;
    }
    sharedDesktop = QGuiApplication::primaryScreen()->geometry();
    const auto positionValue = props.value("position");
    const auto sizeValue = props.value("size");
    if (positionValue.canConvert<QDBusArgument>() && sizeValue.canConvert<QDBusArgument>()) {
        const auto position = qvariant_cast<QDBusArgument>(positionValue);
        const auto size = qvariant_cast<QDBusArgument>(sizeValue);
        if (position.currentSignature() == "(ii)" && size.currentSignature() == "(ii)") {
            int x = 0, y = 0, width = 0, height = 0;
            position.beginStructure();
            position >> x >> y;
            position.endStructure();
            size.beginStructure();
            size >> width >> height;
            size.endStructure();
            if (width > 0 && height > 0)
                sharedDesktop = QRectF(x, y, width, height);
        }
    }
    if (data.contains("restore_token"))
        QSettings().setValue("wayland/restoreToken", data["restore_token"]);
    const auto job = generation;
    auto message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", iface, "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(session)), QVariantMap()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, node, props, job] {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        if (job != generation || session.isEmpty())
            return;
        if (reply.isError()) {
            failSharing("无法连接 PipeWire 远程会话");
            return;
        }
        stream = streamFactory ? streamFactory() : std::make_unique<PipeWireStream>();
        if (!stream) {
            failSharing("无法创建 PipeWire 视频流");
            return;
        }
        connect(stream.get(), &PipeWireStream::failed, this, [this](QString e) {
            failSharing(e);
        });
        QString error;
        const int descriptor = reply.value().isValid() ? ::dup(reply.value().fileDescriptor()) : -1;
        if (descriptor < 0) {
            failSharing("Portal 未返回有效的 PipeWire 连接");
            return;
        }
        if (!stream->start(descriptor, node,
                           props.value("pipewire-serial").toString(), error)) {
            failSharing(error);
            return;
        }
        starting = false;
        active = true;
        emit sharingChanged(true);
        if (capturePending)
            waitForFrame();
    });
#else
    Q_UNUSED(data)
#endif
}
void WaylandCapture::stopSharing() {
    const bool pending = capturePending;
    resetSharing();
    if (pending)
        emit failed("已停止屏幕访问，截图已取消。");
}
void WaylandCapture::failSharing(QString error) {
    resetSharing();
    emit failed(error);
}
void WaylandCapture::resetSharing() {
    ++generation;
    frameTimer.stop();
    starting = false;
    temporarySharing = false;
    capturePending = false;
    sharedDesktop = {};
    if (request) {
        auto r = request;
        request = nullptr;
        r->cancel();
    }
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (stream) {
        stream->stop();
        stream.reset();
    }
#endif
    if (!session.isEmpty()) {
        auto path = session;
        session.clear();
        QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", path,
                                                 "org.freedesktop.portal.Session", "Closed", this,
                                                 SLOT(sessionClosed()));
        auto m = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", path,
                                                "org.freedesktop.portal.Session", "Close");
        QDBusConnection::sessionBus().asyncCall(m);
    }
    bool wasActive = active;
    active = false;
    if (wasActive)
        emit sharingChanged(false);
}
void WaylandCapture::sessionClosed() {
    stopSharing();
}
} // namespace SnapLing

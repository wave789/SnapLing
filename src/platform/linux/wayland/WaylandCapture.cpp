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
WaylandCapture::WaylandCapture(QObject *p) : ICaptureBackend(p) {}
WaylandCapture::~WaylandCapture() {
    cancel();
    stopSharing();
}
QString WaylandCapture::capabilities() const {
    return QString("Screenshot v%1 · ScreenCast v%2 · GlobalShortcuts v%3\nWayland "
                   "的授权对话框可能使原应用弹窗关闭。可主动开启快速共享以减少授权干扰。")
        .arg(PortalRequest::version("org.freedesktop.portal.Screenshot"))
        .arg(PortalRequest::version(iface))
        .arg(PortalRequest::version("org.freedesktop.portal.GlobalShortcuts"));
}
void WaylandCapture::cancel() {
    ++generation;
    if (request) {
        request->cancel();
        request = nullptr;
    }
}
void WaylandCapture::capture(CaptureMode mode, int) {
    if (request) {
        emit failed("已有授权或截图请求，请先完成或取消");
        return;
    }
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (active && stream) {
        auto img = stream->latest();
        if (!img.isNull()) {
            CaptureFrame f;
            f.image = img;
            f.desktop = QRectF(QPointF(0, 0), img.size());
            emit captured(f);
            return;
        }
        emit failed("共享尚未产生帧或已暂停；请稍后重试，或停止共享后使用 Portal 截图。");
        return;
    }
#endif
    ++generation;
    portalCapture(mode, mode == CaptureMode::Window);
}
void WaylandCapture::portalCapture(CaptureMode mode, bool interactive) {
    auto job = generation;
    request = new PortalRequest(this);
    QVariantMap options{{"interactive", interactive}, {"modal", false}};
    if (mode == CaptureMode::Window && PortalRequest::version("org.freedesktop.portal.Screenshot") >= 3)
        options["target"] = uint(2);
    request->call("org.freedesktop.portal.Screenshot", "Screenshot", {QString()}, options,
                  [this, mode, interactive, job](uint code, QVariantMap data, QString error) {
                      request = nullptr;
                      if (job != generation)
                          return;
                      if (code == 2 && !interactive) {
                          portalCapture(mode, true);
                          return;
                      }
                      if (code) {
                          emit failed(error.isEmpty()
                                          ? "系统截图被取消或拒绝。可在系统截图工具中截图，再从剪贴板导入。"
                                          : error);
                          return;
                      }
                      QUrl uri(data["uri"].toString());
                      if (!uri.isLocalFile()) {
                          emit failed("系统未返回有效的本地截图文件");
                          return;
                      }
                      auto *w = new QFutureWatcher<QImage>(this);
                      connect(w, &QFutureWatcher<QImage>::finished, this, [this, w, job] {
                          auto image = w->result();
                          w->deleteLater();
                          if (job != generation)
                              return;
                          if (image.isNull()) {
                              emit failed("系统截图文件无法读取");
                              return;
                          }
                          CaptureFrame f;
                          f.image = image;
                          f.desktop = QRectF(QPointF(0, 0), image.size());
                          emit captured(f);
                      });
                      w->setFuture(QtConcurrent::run(&workerPool, [path = uri.toLocalFile()] {
                          QImage i(path);
                          i.setDevicePixelRatio(1);
                          return i;
                      }));
                  });
}
void WaylandCapture::startSharing() {
#ifndef SNAPLING_HAVE_PIPEWIRE
    emit failed("此构建未启用 PipeWire；可继续使用 Screenshot Portal。");
#else
    if (active || request)
        return;
    request = new PortalRequest(this);
    request->call(iface, "CreateSession", {},
                  {{"session_handle_token", "snapling_" + QUuid::createUuid().toString(QUuid::Id128)}},
                  [this](uint code, QVariantMap data, QString error) {
                      request = nullptr;
                      if (code) {
                          emit failed(error.isEmpty() ? "屏幕共享授权已取消" : error);
                          return;
                      }
                      session = data["session_handle"].toString();
                      if (session.isEmpty()) {
                          emit failed("Portal 未返回会话");
                          return;
                      }
                      QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop", session,
                                                            "org.freedesktop.portal.Session", "Closed", this,
                                                            SLOT(sessionClosed()));
                      selectSources();
                  });
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
    request = new PortalRequest(this);
    request->call(iface, "SelectSources", {QVariant::fromValue(QDBusObjectPath(session))}, opts,
                  [this](uint code, QVariantMap, QString error) {
                      request = nullptr;
                      if (code) {
                          stopSharing();
                          emit failed(error.isEmpty() ? "无法选择屏幕共享来源" : error);
                          return;
                      }
                      startSession();
                  });
}
void WaylandCapture::startSession() {
    request = new PortalRequest(this);
    request->call(iface, "Start", {QVariant::fromValue(QDBusObjectPath(session)), QString()}, {},
                  [this](uint code, QVariantMap data, QString error) {
                      request = nullptr;
                      if (code) {
                          stopSharing();
                          emit failed(error.isEmpty() ? "屏幕共享被拒绝" : error);
                          return;
                      }
                      openRemote(data);
                  });
}
void WaylandCapture::openRemote(const QVariantMap &data) {
#ifdef SNAPLING_HAVE_PIPEWIRE
    auto arg = qvariant_cast<QDBusArgument>(data["streams"]);
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
        stopSharing();
        emit failed("Portal 未返回可用的视频节点");
        return;
    }
    if (data.contains("restore_token"))
        QSettings().setValue("wayland/restoreToken", data["restore_token"]);
    auto message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", iface, "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(session)), QVariantMap()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, node, props] {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        if (session.isEmpty())
            return;
        if (reply.isError()) {
            stopSharing();
            emit failed("无法连接 PipeWire 远程会话");
            return;
        }
        stream = std::make_unique<PipeWireStream>();
        connect(stream.get(), &PipeWireStream::failed, this, [this](QString e) {
            stopSharing();
            emit failed(e);
        });
        QString error;
        if (!stream->start(::dup(reply.value().fileDescriptor()), node,
                           props.value("pipewire-serial").toString(), error)) {
            stopSharing();
            emit failed(error);
            return;
        }
        active = true;
        emit sharingChanged(true);
    });
#else
    Q_UNUSED(data)
#endif
}
void WaylandCapture::stopSharing() {
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

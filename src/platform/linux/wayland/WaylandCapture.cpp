#include "WaylandCapture.h"
#include "Portal.h"
#include "settings/Settings.h"
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
#include <QImageReader>
#include <QScreen>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>
#include <unistd.h>
namespace SnapLing {
static const QString iface = "org.freedesktop.portal.ScreenCast";
static const QString restoreKey = "wayland/multiScreenRestoreToken";
static bool sameLayout(const QVector<CaptureScreen> &a, const QVector<CaptureScreen> &b) {
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].desktop != b[i].desktop || a[i].devicePixelRatio != b[i].devicePixelRatio)
            return false;
    return true;
}
QVector<CaptureScreen> WaylandCapture::currentScreens() const {
    return screenProvider ? screenProvider() : captureScreenLayout();
}
WaylandCapture::WaylandCapture(QObject *p, StreamFactory factory, ScreenProvider screens)
    : ICaptureBackend(p), streamFactory(std::move(factory)), screenProvider(std::move(screens)) {
    method = Settings().value("capture/waylandMethod", "screenshot").toString() == "screencast"
                 ? Method::ScreenCast : Method::Screenshot;
    frameTimer.setInterval(20);
    connect(&frameTimer, &QTimer::timeout, this, &WaylandCapture::deliverFrame);
    auto changed = [this] {
        QSettings().remove(restoreKey);
        if (active || starting || capturePending)
            failSharing("显示器连接或布局已改变，请重新截图。");
    };
    auto watchScreen = [this, changed](QScreen *screen) {
        connect(screen, &QScreen::geometryChanged, this, [changed](const QRect &) { changed(); });
    };
    for (auto *screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [watchScreen, changed](QScreen *screen) {
        watchScreen(screen);
        changed();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [changed](QScreen *) { changed(); });
}
WaylandCapture::~WaylandCapture() {
    resetSharing();
}
QString WaylandCapture::capabilities() const {
    return "默认一次捕获完整桌面，在各显示器上冻结画面并直接框选。首次使用可能需要系统截图授权。";
}
void WaylandCapture::setMethod(Method value) {
    if (method == value)
        return;
    resetSharing();
    method = value;
}
void WaylandCapture::cancel() {
    if (temporarySharing || starting || request)
        resetSharing();
    else {
        ++generation;
        capturePending = false;
        composing = false;
        frameTimer.stop();
#ifdef SNAPLING_HAVE_PIPEWIRE
        for (auto &source : sources)
            source.stream->setActive(false);
#endif
    }
}
void WaylandCapture::capture(CaptureMode, int screen) {
    if (capturePending) {
        emit failed("已有截图请求，请先完成或取消");
        return;
    }
    const auto screens = currentScreens();
    if (screens.isEmpty() || screens.size() > 16 || screen >= screens.size()) {
        emit failed("所选显示器已断开，请重新选择显示器");
        return;
    }
    if ((active || starting) && !sameLayout(layout, screens)) {
        resetSharing();
        QSettings().remove(restoreKey);
    }
    layout = screens;
    allScreens = screen < 0;
    requestedScreen = allScreens ? QString() : screens[screen].name;
    capturePending = true;
    if (method == Method::Screenshot) {
        ++generation;
        frameWait.start();
        qInfo("capture_backend=screenshot screens=%lld target=%d", qlonglong(layout.size()), screen);
        portalCapture();
        return;
    }
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (active && !sources.empty()) {
        if (!requestedSourcesAvailable())
            return;
        waitForFrame();
        return;
    }
    if (!starting)
        beginSharing(true);
#else
    failSharing("此构建未启用 PipeWire，请在设置中使用系统单次截图。");
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
        qInfo("portal_screenshot_response code=%u has_uri=%d", code, data.contains("uri"));
        if (code) {
            failSharing(error.isEmpty() ? (code == 1 ? "系统截图授权已取消（Portal 响应码 1）。"
                : QString("系统未提供截图（Portal 响应码 %1）。可能是截图权限被拒绝或桌面截图服务出错。若曾拒绝授权，系统可能直接拒绝后续请求而不再弹窗。").arg(code))
                                         : "系统截图失败：" + error);
            return;
        }
        const QUrl uri(data["uri"].toString());
        if (!uri.isLocalFile()) {
            failSharing("系统未返回有效的本地截图文件");
            return;
        }
        struct Result { CaptureFrame frame; QString error; QSize pixels; QRectF desktop; };
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, job] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (job != generation)
                return;
            qInfo("portal_screenshot_pixels=%dx%d desktop=%.0f,%.0f,%.0fx%.0f",
                  result.pixels.width(), result.pixels.height(), result.desktop.x(), result.desktop.y(),
                  result.desktop.width(), result.desktop.height());
            if (!sameLayout(layout, currentScreens())) {
                failSharing("截图期间显示器布局已改变，请重新截图。");
                return;
            }
            if (!result.error.isEmpty()) {
                failSharing(result.error);
                return;
            }
            result.frame.captureMs = frameWait.elapsed();
            capturePending = false;
            emit captured(result.frame);
        });
        watcher->setFuture(QtConcurrent::run(&workerPool, [path = uri.toLocalFile(), screens = layout,
                                                          target = requestedScreen, all = allScreens] {
            Result result;
            QImageReader reader(path);
            const auto size = reader.size();
            result.pixels = size;
            if (size.isEmpty() || qint64(size.width()) * size.height() > 100000000) {
                result.error = "系统截图尺寸无效或超过 1 亿像素限制";
                return result;
            }
            QImage image = reader.read();
            image.setDevicePixelRatio(1);
            if (image.isNull()) {
                result.error = "系统截图文件无法读取";
                return result;
            }
            CaptureFrame atlas;
            atlas.image = image;
            for (const auto &screen : screens) {
                if (screen.desktop.isEmpty()) {
                    result.error = "显示器布局无效，请重新截图";
                    return result;
                }
                atlas.desktop = atlas.desktop.united(screen.desktop);
            }
            result.desktop = atlas.desktop;
            // Mutter 的桌面截图使用统一的 stage 像素比例，不能把主屏图像误认为完整桌面。
            const qreal scale = image.width() / atlas.desktop.width();
            if (!qIsFinite(scale) || qAbs(image.height() - atlas.desktop.height() * scale) > 2) {
                result.error = "系统截图与显示器布局不匹配。请检查显示器缩放，或在设置中选择兼容截图。";
                return result;
            }
            for (auto screen : screens) {
                if (!all && screen.name != target)
                    continue;
                screen.image = image.copy(atlas.pixelRect(screen.desktop));
                screen.devicePixelRatio = scale;
                result.frame.screens << std::move(screen);
            }
            if (result.frame.screens.isEmpty()) {
                result.error = "所选显示器已断开，请重新截图";
                return result;
            }
            result.frame.screenName = result.frame.screens.first().name;
            result.frame.desktop = all ? atlas.desktop : result.frame.screens.first().desktop;
            result.frame.image = all ? image : result.frame.screens.first().image;
            return result;
        }));
    });
}
void WaylandCapture::startSharing() {
    if (!active && !starting) {
        layout = currentScreens();
        beginSharing(false);
    }
}
void WaylandCapture::resetScreenSelection() {
    const bool pending = capturePending;
    resetSharing();
    QSettings settings;
    settings.remove(restoreKey);
    settings.remove("wayland/restoreToken");
    if (pending)
        emit failed("已取消当前截图，请重新选择兼容截图的屏幕。");
}
void WaylandCapture::beginSharing(bool temporary) {
#ifndef SNAPLING_HAVE_PIPEWIRE
    Q_UNUSED(temporary)
    failSharing("此构建未启用 PipeWire，请安装 libpipewire-0.3-dev 并重新构建。");
#else
    starting = true;
    temporarySharing = temporary;
    const auto job = ++generation;
    PortalRequest::queryVersion(iface, this, [this, job](uint version, QString error) {
        if (job != generation)
            return;
        if (!version) {
            failSharing(error.isEmpty() ? "系统未提供兼容截图接口 ScreenCast" : error);
            return;
        }
        screenCastVersion = version;
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
                "org.freedesktop.portal.Session", "Closed", this, SLOT(sessionClosed(QDBusMessage)));
            selectSources();
        });
    });
#endif
}
void WaylandCapture::waitForFrame() {
#ifdef SNAPLING_HAVE_PIPEWIRE
    for (auto &source : sources)
        source.stream->setActive(allScreens || source.screen.name == requestedScreen);
#endif
    frameWait.start();
    frameTimer.start();
    deliverFrame();
}
void WaylandCapture::deliverFrame() {
#ifdef SNAPLING_HAVE_PIPEWIRE
    if (!capturePending || !active || sources.empty() || composing)
        return;
    if (!sameLayout(layout, currentScreens())) {
        QSettings().remove(restoreKey);
        failSharing("显示器布局已改变，请重新截图并选择共享屏幕。");
        return;
    }
    QVector<CaptureScreen> frames;
    for (const auto &source : sources) {
        if (!allScreens && source.screen.name != requestedScreen)
            continue;
        auto frame = source.screen;
        frame.image = source.stream->latest();
        if (frame.image.isNull()) {
            if (frameWait.elapsed() >= 10000)
                failSharing("10 秒内未获得所有所选显示器的画面，请检查屏幕共享服务并重新选择屏幕。");
            return;
        }
        source.stream->setActive(false);
        frames << frame;
    }
    composing = true;
    frameTimer.stop();
    const auto job = generation;
    struct Result { CaptureFrame frame; QString error; };
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, job] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (job != generation)
            return;
        composing = false;
        if (!result.error.isEmpty()) {
            failSharing(result.error);
            return;
        }
        result.frame.captureMs = frameWait.elapsed();
        capturePending = false;
        if (temporarySharing)
            resetSharing();
        emit captured(result.frame);
    });
    watcher->setFuture(QtConcurrent::run(&workerPool, [frames = std::move(frames)]() mutable {
        Result result;
        result.frame = combineCaptureScreens(std::move(frames), result.error);
        return result;
    }));
#endif
}
bool WaylandCapture::requestedSourcesAvailable() {
#ifdef SNAPLING_HAVE_PIPEWIRE
    QSet<QString> names;
    for (const auto &source : sources)
        names.insert(source.screen.name);
    QStringList missing;
    if (allScreens) {
        for (const auto &screen : layout)
            if (!names.contains(screen.name))
                missing << screen.name;
    } else if (!names.contains(requestedScreen))
        missing << requestedScreen;
    if (!missing.isEmpty()) {
        QSettings().remove(restoreKey);
        failSharing("尚未授权这些显示器：" + missing.join("、") +
                    "。请点击“重置兼容截图授权”，在系统授权窗口中勾选需要的内置和外接屏幕。");
        return false;
    }
#endif
    return true;
}
void WaylandCapture::selectSources() {
    QVariantMap opts{{"types", uint(1)}, {"multiple", true}, {"cursor_mode", uint(1)}};
    if (screenCastVersion >= 4) {
        opts["persist_mode"] = uint(2);
        QString token = QSettings().value(restoreKey).toString();
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
    struct Node { uint id = 0; QVariantMap properties; };
    std::vector<Node> nodes;
    QSet<uint> ids;
    arg.beginArray();
    while (!arg.atEnd()) {
        Node node;
        arg.beginStructure();
        arg >> node.id >> node.properties;
        arg.endStructure();
        if (!node.id || ids.contains(node.id) || nodes.size() >= 16) {
            failSharing("Portal 返回的视频节点无效或重复");
            return;
        }
        ids.insert(node.id);
        nodes.push_back(std::move(node));
    }
    arg.endArray();
    if (nodes.empty()) {
        failSharing("Portal 未返回可用的视频节点");
        return;
    }
    auto pair = [](const QVariant &value, QPoint &point) {
        const auto argument = qvariant_cast<QDBusArgument>(value);
        if (argument.currentSignature() != "(ii)")
            return false;
        int x = 0, y = 0;
        argument.beginStructure();
        argument >> x >> y;
        argument.endStructure();
        point = {x, y};
        return true;
    };
    QSet<QString> used;
    for (const auto &node : nodes) {
        const auto &props = node.properties;
        if (props.contains("source_type") && props.value("source_type").toUInt() != 1) {
            failSharing("系统返回的来源不是显示器，请重新选择共享屏幕。");
            return;
        }
        QPoint position, size;
        const bool geometryKnown = pair(props.value("position"), position) && pair(props.value("size"), size) &&
                                   size.x() > 0 && size.y() > 0;
        const QRectF geometry(position, QSizeF(size.x(), size.y()));
        int match = -1;
        for (qsizetype i = 0; i < layout.size(); ++i) {
            if (used.contains(layout[i].name))
                continue;
            if (geometryKnown && qAbs(layout[i].desktop.x() - geometry.x()) <= 1 &&
                qAbs(layout[i].desktop.y() - geometry.y()) <= 1 &&
                qAbs(layout[i].desktop.width() - geometry.width()) <= 1 &&
                qAbs(layout[i].desktop.height() - geometry.height()) <= 1) {
                match = int(i);
                break;
            }
            if (!geometryKnown && !layout[i].name.isEmpty() &&
                (props.value("id").toString() == layout[i].name ||
                 props.value("mapping_id").toString() == layout[i].name))
                match = int(i);
        }
        if (match < 0 && layout.size() == 1 && nodes.size() == 1)
            match = 0;
        if (match < 0) {
            QSettings().remove(restoreKey);
            failSharing("无法将共享画面匹配到当前显示器。请重新选择共享屏幕；系统需要提供屏幕位置或名称。");
            return;
        }
        SharedSource source;
        source.node = node.id;
        source.serial = props.value("pipewire-serial").toString();
        source.screen = layout[match];
        if (geometryKnown)
            source.screen.desktop = geometry;
        used.insert(source.screen.name);
        sources.push_back(std::move(source));
    }
    if (capturePending && !requestedSourcesAvailable())
        return;
    if (temporarySharing && !allScreens)
        std::erase_if(sources, [this](const auto &source) { return source.screen.name != requestedScreen; });
    if (data.contains("restore_token"))
        QSettings().setValue(restoreKey, data["restore_token"]);
    const auto job = generation;
    opening = int(sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        // 每一路流单独打开远程连接；不能让多个 PipeWire core 共享 dup 后的同一协议 socket。
        auto message = QDBusMessage::createMethodCall(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", iface, "OpenPipeWireRemote");
        message.setArguments({QVariant::fromValue(QDBusObjectPath(session)), QVariantMap()});
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, index, job] {
            QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
            watcher->deleteLater();
            if (job != generation || session.isEmpty())
                return;
            if (reply.isError()) {
                failSharing("无法连接 PipeWire 远程会话");
                return;
            }
            auto &source = sources[index];
            source.stream = streamFactory ? streamFactory() : std::make_unique<PipeWireStream>();
            if (!source.stream) {
                failSharing("无法创建 PipeWire 视频流");
                return;
            }
            const QPointer<PipeWireStream> stream(source.stream.get());
            connect(stream, &PipeWireStream::failed, this, [this, stream](QString error) {
                if (stream)
                    failSharing(error);
            });
            QString error;
            const int descriptor = reply.value().isValid() ? ::dup(reply.value().fileDescriptor()) : -1;
            if (descriptor < 0) {
                failSharing("Portal 未返回有效的 PipeWire 连接");
                return;
            }
            if (!source.stream->start(descriptor, source.node, source.serial, error)) {
                failSharing(error);
                return;
            }
            if (--opening == 0) {
                starting = false;
                active = true;
                emit sharingChanged(true);
                if (capturePending)
                    waitForFrame();
            }
        });
    }
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
    qWarning("capture_failed backend=wayland reason=%s", qPrintable(error));
    resetSharing();
    emit failed(error);
}
void WaylandCapture::resetSharing() {
    ++generation;
    frameTimer.stop();
    starting = false;
    temporarySharing = false;
    capturePending = false;
    composing = false;
    layout.clear();
    if (request) {
        auto r = request;
        request = nullptr;
        r->cancel();
    }
#ifdef SNAPLING_HAVE_PIPEWIRE
    for (auto &source : sources)
        if (source.stream)
            source.stream->stop();
    sources.clear();
    opening = 0;
#endif
    if (!session.isEmpty()) {
        auto path = session;
        session.clear();
        QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", path,
                                                 "org.freedesktop.portal.Session", "Closed", this,
                                                 SLOT(sessionClosed(QDBusMessage)));
        auto m = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", path,
                                                "org.freedesktop.portal.Session", "Close");
        QDBusConnection::sessionBus().asyncCall(m);
    }
    bool wasActive = active;
    active = false;
    if (wasActive)
        emit sharingChanged(false);
}
void WaylandCapture::sessionClosed(const QDBusMessage &message) {
    if (message.path() == session)
        stopSharing();
}
} // namespace SnapLing

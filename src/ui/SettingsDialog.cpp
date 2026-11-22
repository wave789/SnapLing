#include "SettingsDialog.h"
#include "ocr/OcrEngine.h"
#include "platform/SecretStore.h"
#ifndef Q_OS_WIN
#include "ocr/RapidOcr.h"
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTimer>
#endif
#include "platform/Autostart.h"
#include "translation/TranslationProvider.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
namespace SnapLing {
namespace {
QString baiduTranslateEndpoint() {
    return qEnvironmentVariable("SNAPLING_BAIDU_ENDPOINT",
                                "https://fanyi-api.baidu.com/api/trans/vip/translate");
}
} // namespace
#ifndef Q_OS_WIN
namespace {
void addGnomeScreenshotAcceleration(QFormLayout *form) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *button = new QPushButton("启用", row);
    button->setObjectName("gnomeScreenshotAccelerationButton");
    button->setEnabled(false);
    auto *status = new QLabel("正在检查 GNOME 扩展…", row);
    status->setObjectName("gnomeScreenshotAccelerationStatus");
    status->setWordWrap(true);
    layout->addWidget(button);
    layout->addWidget(status, 1);
    form->addRow("GNOME 即时框选", row);

    const auto call = [row](const QString &method, auto handler) {
        auto message = QDBusMessage::createMethodCall("org.gnome.Shell",
                                                     "/org/gnome/Shell",
                                                     "org.gnome.Shell.Extensions", method);
        message << QStringLiteral("snapling-no-flash@snapling.local");
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 2500), row);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, row, [watcher, handler] {
            handler(*watcher);
            watcher->deleteLater();
        });
    };
    const auto refresh = [call, button, status] {
        button->setEnabled(false);
        call("GetExtensionInfo", [button, status](const QDBusPendingCall &pending) {
            const QDBusPendingReply<QVariantMap> reply = pending;
            if (reply.isError()) {
                status->setText("无法读取 GNOME 扩展状态。");
                return;
            }
            const auto info = reply.value();
            if (info.isEmpty()) {
                status->setText("GNOME 尚未发现提速扩展。若刚安装 DEB，请注销后重新登录。");
                return;
            }
            const auto version = info.value("version").toDouble();
            if (version < 2) {
                status->setText("当前会话加载了旧版透明白闪扩展。请按项目说明更新后重新登录。");
                return;
            }

            // org.gnome.Shell.Extensions reports the requested preference separately from the live state.
            const int state = info.value("state").toInt();
            const bool active = state == 1;
            const bool enabled = info.value("enabled", active).toBool();
            button->setProperty("extensionEnabled", enabled);
            button->setText(enabled ? "停用" : "启用");
            button->setEnabled(state != 4);
            if (state == 4)
                status->setText("此扩展不支持当前 GNOME 版本。");
            else if (state == 3)
                status->setText("扩展加载失败。可在 GNOME 扩展应用中查看错误。");
            else if (active && version >= 4)
                status->setText("已启用；主截图快捷键默认在 GNOME 合成器中立即显示选区。");
            else if (active && version >= 3)
                status->setText("已跳过截图闪光和 SnapLing 选区动画。");
            else if (active)
                status->setText("已跳过截图闪光。新版扩展还可跳过选区动画，请更新后重新登录。");
            else if (enabled)
                status->setText("已请求启用，GNOME 尚未激活扩展。");
            else
                status->setText("建议启用；安装新版后 SnapLing 会自动尝试启用即时框选。");
        });
    };
    QObject::connect(button, &QPushButton::clicked, row, [call, refresh, button, status] {
        button->setEnabled(false);
        status->setText("正在更新 GNOME 扩展状态…");
        call(button->property("extensionEnabled").toBool() ? "DisableExtension" : "EnableExtension",
             [refresh, button, status](const QDBusPendingCall &pending) {
                 const QDBusPendingReply<bool> reply = pending;
                 if (reply.isError() || !reply.value()) {
                     status->setText("GNOME 未能更新提速扩展。");
                     button->setEnabled(true);
                     return;
                 }
                 QTimer::singleShot(300, button, refresh);
             });
    });
    refresh();
}
} // namespace
#endif

SettingsDialog::SettingsDialog(Settings &s, ProviderConfig &runtime, QWidget *parent) : QDialog(parent) {
    setWindowTitle("SnapLing 设置");
    resize(660, 560);
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto page = [&](QString name) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto *view = new QScrollArea;
        view->setWidgetResizable(true);
        view->setWidget(w);
        tabs->addTab(view, name);
        return f;
    };
    auto *general = page("通用");
    auto *theme = new QComboBox;
    theme->addItems({"跟随系统", "浅色", "深色"});
    theme->setCurrentIndex(s.value("general/theme", 0).toInt());
    general->addRow("主题", theme);
    auto *captureMethod = new QComboBox(this);
    const bool gnome = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains("GNOME", Qt::CaseInsensitive);
    if (gnome) {
        captureMethod->addItem("GNOME 即时框选（推荐，立即选屏）", "compositor");
        captureMethod->setItemData(0,
                                   "GNOME 合成器先冻结桌面再显示选区；GNOME 快速框选不可用时自动回退。",
                                   Qt::ToolTipRole);
    }
    captureMethod->addItem("系统单次截图（兼容回退，无持续共享）", "screenshot");
    captureMethod->addItem("兼容截图（需要系统选择共享屏幕，取图后结束）", "screencast");
    const auto defaultMethod = gnome ? QStringLiteral("compositor") : QStringLiteral("screenshot");
    captureMethod->setCurrentIndex(qMax(0, captureMethod->findData(s.value("capture/waylandMethod", defaultMethod))));
    if (QGuiApplication::platformName().contains("wayland") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland") {
        general->addRow("截图方式", captureMethod);
#ifndef Q_OS_WIN
        if (gnome)
            addGnomeScreenshotAcceleration(general);
#endif
    } else
        captureMethod->hide();
    auto *startup = new QCheckBox("登录后自动启动");
    startup->setChecked(s.value("general/startup", false).toBool());
    general->addRow(startup);
    auto *background = new QCheckBox("关闭主窗口后继续运行");
    background->setChecked(s.value("general/background", true).toBool());
    general->addRow(background);
    auto *autoCopy = new QCheckBox("截图后自动复制图片");
    autoCopy->setChecked(s.value("general/autoCopy", false).toBool());
    general->addRow(autoCopy);
    auto *closeCopy = new QCheckBox("复制后关闭编辑器");
    closeCopy->setChecked(s.value("general/closeAfterCopy", false).toBool());
    general->addRow(closeCopy);
    auto *saveDir = new QLineEdit(s.value("general/saveDir", QDir::homePath() + "/Pictures").toString());
    general->addRow("保存目录", saveDir);
    auto *format = new QComboBox;
    format->addItems({"PNG", "JPG"});
    format->setCurrentText(s.value("general/format", "PNG").toString());
    general->addRow("默认格式", format);
    auto *quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(s.value("general/quality", 92).toInt());
    general->addRow("JPG 质量", quality);
    auto *jpgBg = new QLineEdit(s.value("general/jpegBackground", "#ffffff").toString());
    general->addRow("JPG 背景色", jpgBg);
    auto *history = new QCheckBox("启用历史（默认关闭；仅在导出时记录）");
    history->setChecked(s.value("history/enabled", false).toBool());
    general->addRow(history);
    auto *hotkeys = page("快捷键");
    auto *key = new QKeySequenceEdit(QKeySequence(s.value("hotkey/capture", "Ctrl+Alt+A").toString()));
    hotkeys->addRow("截图", key);
    auto extraKeys = std::make_shared<QMap<QString, QKeySequenceEdit *>>();
    const QList<QPair<QString, QString>> hotkeyNames{
        {"full", "全屏"}, {"window", "窗口"}, {"delay", "延时 3 秒"}, {"ocr", "OCR"}, {"translate", "翻译"}};
    for (auto [id, label] : hotkeyNames) {
        auto *edit = new QKeySequenceEdit(QKeySequence(s.value("hotkey/" + id).toString()));
        (*extraKeys)[id] = edit;
        hotkeys->addRow(label + "（可留空）", edit);
    }

    auto *hint = new QLabel("Wayland 快捷键由系统授权。若未成功注册，在 Ubuntu 设置 → 键盘 → 自定义快捷键中绑定：\n\"" +
                            QCoreApplication::applicationFilePath() + "\" --capture\n主界面可复制此命令，也可重新注册快捷键。");
    hint->setWordWrap(true);
    hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hotkeys->addRow(hint);
    auto *ocr = page("OCR");
    ocr->addRow("识别引擎", new QLabel("PaddleOCR · PP-OCRv6 Small（内置）"));
    auto *paddleStatus = new QLabel;
    paddleStatus->setObjectName("paddleOcrStatus");
    paddleStatus->setWordWrap(true);
    paddleStatus->setTextFormat(Qt::PlainText);
    paddleStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
#ifndef Q_OS_WIN
    const QFileInfo launcher(RapidOcr::launcher(RapidOcr::Profile::Paddle));
    const QFileInfo detector(RapidOcr::modelDirectory() + "/det/PP-OCRv6_small_det.onnx");
    const QFileInfo recognizer(RapidOcr::modelDirectory() + "/rec/PP-OCRv6_small_rec.onnx");
    const QFileInfo detectorConfig(RapidOcr::modelDirectory() + "/det/inference.yml");
    const QFileInfo recognizerConfig(RapidOcr::modelDirectory() + "/rec/inference.yml");
    const bool ready = launcher.isExecutable() && detector.isFile() && recognizer.isFile() &&
                       detectorConfig.isFile() && recognizerConfig.isFile();
    paddleStatus->setText(
        QString(ready ? "模型、配置和本地推理环境已随 SnapLing 安装，无需下载或选择目录。\n"
                      : "正式安装包内置完整 OCR 运行环境；当前开发构建尚未生成运行时。\n") +
        "模型目录：" + RapidOcr::modelDirectory() + "\n运行环境：" + launcher.absolutePath());
#else
    paddleStatus->setText("当前平台未提供 PaddleOCR 运行时。");
#endif
    ocr->addRow(paddleStatus);
    auto *ai = page("翻译服务");
    auto *preset = new QComboBox;
    preset->setObjectName("translationBackend");
    preset->addItem("本地 Ollama", int(TranslationBackend::OpenAiCompatible));
    preset->addItem("OpenAI 兼容服务", int(TranslationBackend::OpenAiCompatible));
    preset->addItem("百度通用文本翻译", int(TranslationBackend::BaiduGeneral));
    const auto initialPreset = runtime.backend == TranslationBackend::BaiduGeneral   ? 2
                               : runtime.isLocal()                                  ? 0
                                                                                    : 1;
    preset->setCurrentIndex(initialPreset);
    ai->addRow("服务类型", preset);
    auto *name = new QLineEdit(runtime.name), *url = new QLineEdit(runtime.baseUrl),
         *model = new QLineEdit(runtime.model), *env = new QLineEdit(runtime.keyEnv),
         *appId = new QLineEdit(runtime.appId), *secret = new QLineEdit(runtime.sessionKey),
         *language = new QLineEdit(runtime.targetLanguage);
    name->setObjectName("translationProviderName");
    env->setObjectName("translationKeyEnv");
    secret->setObjectName("translationSecret");
    appId->setObjectName("baiduAppId");
    appId->setPlaceholderText("百度翻译控制台中的 AppID");
    secret->setEchoMode(QLineEdit::Password);
    secret->setPlaceholderText("保存到系统密钥环；留空可使用已保存密钥或环境变量");
    if (runtime.backend == TranslationBackend::BaiduGeneral && secret->text().isEmpty())
        secret->setText(SecretStore::loadBaiduTranslationSecret(runtime.appId));
    connect(appId, &QLineEdit::textEdited, this, [=] {
        if (!secret->isModified())
            secret->setText(SecretStore::loadBaiduTranslationSecret(appId->text()));
    });
    ai->addRow("名称", name);
    ai->addRow("Base URL", url);
    ai->addRow("模型", model);
    ai->addRow("百度 AppID", appId);
    ai->addRow("密钥环境变量", env);
    ai->addRow("密钥", secret);
    ai->addRow("目标语言", language);
    auto *disableThinking = new QCheckBox("关闭思考模式（加快翻译）");
    disableThinking->setChecked(runtime.disableThinking);
    disableThinking->setToolTip(
        "适用于 Ollama 和支持 reasoning_effort 的兼容服务。服务不支持此参数时，请取消勾选。");
    auto *timeout = new QSpinBox;
    timeout->setRange(1, 600);
    timeout->setValue(runtime.timeoutSeconds);
    ai->addRow("超时（秒）", timeout);
    auto *tokens = new QSpinBox;
    tokens->setRange(128, 65536);
    tokens->setValue(runtime.maxTokens);
    ai->addRow("最大输出 token", tokens);
    auto *temp = new QDoubleSpinBox;
    temp->setRange(0, 2);
    temp->setSingleStep(.1);
    temp->setValue(runtime.temperature);
    ai->addRow("Temperature", temp);
    auto *json = new QCheckBox("发送 response_format: json_object");
    json->setChecked(runtime.structuredJson);
    ai->addRow(json);
    ai->addRow(disableThinking);
    auto applyBackendVisibility = [=] {
        const bool baidu =
            TranslationBackend(preset->currentData().toInt()) == TranslationBackend::BaiduGeneral;
        ai->setRowVisible(url, !baidu);
        ai->setRowVisible(model, !baidu);
        ai->setRowVisible(tokens, !baidu);
        ai->setRowVisible(temp, !baidu);
        ai->setRowVisible(json, !baidu);
        ai->setRowVisible(disableThinking, !baidu);
        ai->setRowVisible(appId, baidu);
        ai->setRowVisible(env, !baidu);
    };
    connect(preset, qOverload<int>(&QComboBox::activated), this, [=](int index) {
        const bool baidu =
            TranslationBackend(preset->itemData(index).toInt()) == TranslationBackend::BaiduGeneral;
        if (baidu) {
            name->setText("百度通用文本翻译");
            url->setText(baiduTranslateEndpoint());
            model->clear();
            if (env->text().trimmed().isEmpty() || env->text() == "SNAPLING_API_KEY")
                env->setText("BAIDU_TRANSLATE_SECRET_KEY");
            if (language->text().trimmed().isEmpty())
                language->setText("简体中文");
            secret->setText(SecretStore::loadBaiduTranslationSecret(appId->text()));
        } else if (index == 0) {
            secret->clear();
            name->setText("本地 Ollama");
            url->setText("http://127.0.0.1:11434/v1");
            model->setText("qwen3.5:4b");
            env->setText("SNAPLING_API_KEY");
            disableThinking->setChecked(true);
        } else {
            secret->clear();
            name->setText("OpenAI 兼容服务");
            url->clear();
            model->clear();
            if (env->text() == "BAIDU_TRANSLATE_SECRET_KEY")
                env->setText("SNAPLING_API_KEY");
            disableThinking->setChecked(false);
        }
        applyBackendVisibility();
    });
    applyBackendVisibility();
    auto config = [=] {
        ProviderConfig p;
        p.backend = TranslationBackend(preset->currentData().toInt());
        p.name = name->text().trimmed();
        p.baseUrl = p.backend == TranslationBackend::BaiduGeneral
                        ? baiduTranslateEndpoint()
                        : url->text().trimmed();
        p.model = model->text().trimmed();
        p.keyEnv = p.backend == TranslationBackend::BaiduGeneral
                       ? QStringLiteral("BAIDU_TRANSLATE_SECRET_KEY")
                       : env->text().trimmed();
        p.appId = appId->text().trimmed();
        p.sessionKey = p.backend == TranslationBackend::BaiduGeneral ? secret->text().trimmed() : secret->text();
        p.targetLanguage = language->text().trimmed();
        p.timeoutSeconds = timeout->value();
        p.maxTokens = tokens->value();
        p.temperature = temp->value();
        p.structuredJson = json->isChecked();
        p.disableThinking = disableThinking->isChecked();
        return p;
    };
    auto profiles =
        std::make_shared<QJsonArray>(QJsonDocument::fromJson(s.value("ai/profiles").toByteArray()).array());
    auto *profilesBox = new QComboBox;
    profilesBox->setObjectName("translationProfiles");
    auto refreshProfiles = [=] {
        profilesBox->clear();
        for (auto v : *profiles)
            profilesBox->addItem(v.toObject()["name"].toString());
    };
    refreshProfiles();
    ai->insertRow(0, "已保存供应商", profilesBox);
    connect(profilesBox, qOverload<int>(&QComboBox::activated), this, [=](int i) {
        if (i < 0 || i >= profiles->size())
            return;
        auto o = profiles->at(i).toObject();
        auto backend = translationBackendFromId(o["backend"].toString());
        if (!o.contains("backend") &&
            (o["name"].toString().contains("百度") || o["url"].toString().contains("baidu", Qt::CaseInsensitive)))
            backend = TranslationBackend::BaiduGeneral;
        int presetIndex = o.value("preset").toInt(-1);
        if (presetIndex < 0)
            presetIndex = backend == TranslationBackend::BaiduGeneral
                              ? 2
                              : (o["url"].toString().contains("127.0.0.1") ||
                                         o["url"].toString().contains("localhost")
                                     ? 0
                                     : 1);
        preset->setCurrentIndex(qBound(0, presetIndex, preset->count() - 1));
        name->setText(o["name"].toString());
        url->setText(o["url"].toString());
        model->setText(o["model"].toString());
        appId->setText(o["appId"].toString());
        env->setText(o["keyEnv"].toString());
        language->setText(o["language"].toString());
        timeout->setValue(o["timeout"].toInt(90));
        tokens->setValue(o["tokens"].toInt(4096));
        temp->setValue(o["temperature"].toDouble(.1));
        json->setChecked(o["json"].toBool(true));
        disableThinking->setChecked(o["disableThinking"].toBool(
            o["url"].toString().contains("127.0.0.1") || o["url"].toString().contains("localhost")));
        applyBackendVisibility();
        secret->clear();
        if (backend == TranslationBackend::BaiduGeneral) {
            secret->setText(SecretStore::loadBaiduTranslationSecret(o["appId"].toString()));
            secret->setPlaceholderText("留空则不修改已保存密钥");
        }
    });
    auto *profileButtons = new QWidget;
    auto *profileRow = new QHBoxLayout(profileButtons);
    profileRow->setContentsMargins(0, 0, 0, 0);
    auto *remember = new QPushButton("添加 / 更新供应商");
    auto *forget = new QPushButton("删除所选供应商");
    profileRow->addWidget(remember);
    profileRow->addWidget(forget);
    ai->addRow(profileButtons);
    connect(remember, &QPushButton::clicked, this, [=, this] {
        auto p = config();
        if (p.name.isEmpty())
            return;
        if (p.backend == TranslationBackend::BaiduGeneral && !p.sessionKey.isEmpty()) {
            QString error;
            if (!SecretStore::store(SecretStore::baiduTranslationCredential(p.appId), p.sessionKey, &error)) {
                QMessageBox::warning(this, "密钥未保存", error);
                return;
            }
        }
        QJsonObject o{{"backend", translationBackendId(p.backend)},
                      {"preset", preset->currentIndex()},
                      {"name", p.name},
                      {"url", p.baseUrl},
                      {"model", p.model},
                      {"keyEnv", p.keyEnv},
                      {"appId", p.appId},
                      {"language", p.targetLanguage},
                      {"timeout", p.timeoutSeconds},
                      {"tokens", p.maxTokens},
                      {"temperature", p.temperature},
                      {"json", p.structuredJson},
                      {"disableThinking", p.disableThinking}};
        int found = -1;
        for (int i = 0; i < profiles->size(); ++i)
            if (profiles->at(i).toObject()["name"] == p.name)
                found = i;
        if (found >= 0)
            (*profiles)[found] = o;
        else
            profiles->append(o);
        refreshProfiles();
        profilesBox->setCurrentText(p.name);
    });
    connect(forget, &QPushButton::clicked, this, [=] {
        int i = profilesBox->currentIndex();
        if (i >= 0 && i < profiles->size()) {
            profiles->removeAt(i);
            refreshProfiles();
        }
    });
    auto *test = new QPushButton("测试连接");
    ai->addRow(test);
    auto *openAiProvider = new OpenAiTranslationProvider(this);
    auto *baiduProvider = new BaiduTranslationProvider(this);
    connect(test, &QPushButton::clicked, this, [this, openAiProvider, baiduProvider, config] {
        const auto candidate = config();
        if (candidate.backend == TranslationBackend::BaiduGeneral) {
            if (candidate.appId.isEmpty()) {
                QMessageBox::warning(this, "百度翻译",
                                     "百度 AppID 为空，请确认已填写“百度 AppID”。");
                return;
            }
            const auto environmentSecret =
                qEnvironmentVariable(candidate.keyEnv.toUtf8().constData());
            if (candidate.sessionKey.isEmpty() && environmentSecret.isEmpty() &&
                SecretStore::loadBaiduTranslationSecret(candidate.appId).isEmpty()) {
                QMessageBox::warning(this, "百度翻译",
                                     "百度密钥为空，请填写“密钥”，或设置环境变量 "
                                     "BAIDU_TRANSLATE_SECRET_KEY。");
                return;
            }
            TranslationUnit unit;
            unit.id = "connection-test";
            unit.source = "Hello";
            unit.box = {0, 0, 100, 20};
            unit.language = candidate.targetLanguage;
            baiduProvider->translate({unit}, candidate);
        } else {
            openAiProvider->testConnection(candidate);
        }
    });
    connect(openAiProvider, &OpenAiTranslationProvider::connectionResult, this,
            [this](bool, QString message) { QMessageBox::information(this, "连接测试", message); });
    connect(baiduProvider, &ITranslationProvider::completed, this,
            [this](auto units, qint64) {
                QMessageBox::information(
                    this, "连接测试",
                    "百度翻译连接成功，测试译文：" + (units.isEmpty() ? QString() : units.first().target));
            });
    connect(baiduProvider, &ITranslationProvider::failed, this,
            [this](QString message) { QMessageBox::warning(this, "连接测试", message); });
    auto *privacy =
        new QLabel("供应商类型和百度 AppID 会保存；百度密钥按 AppID 写入系统密钥环。"
                   "使用当前填写的密钥，其次使用已保存密钥；均为空时读取环境变量。"
                   "截图文本仅在主动翻译时发送到所选服务。");
    privacy->setWordWrap(true);
    ai->addRow(privacy);
    auto *performance = page("性能");
    auto *cache = new QCheckBox("缓存 OCR 模型以加速连续识别");
    cache->setChecked(s.value("ocr/cache", true).toBool());
    performance->addRow(cache);
    auto *days = new QSpinBox;
    days->setRange(1, 3650);
    days->setValue(s.value("history/days", 30).toInt());
    performance->addRow("历史保留天数", days);
    auto *limit = new QSpinBox;
    limit->setRange(16, 102400);
    limit->setValue(s.value("history/megabytes", 512).toInt());
    performance->addRow("历史空间 MiB", limit);
    performance->addRow(
        new QLabel("快速屏幕共享需要在主界面主动授权；退出自动停止。\n共享仅缓存最新一帧，不保存屏幕流。"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this,
            [&s, &runtime, this, config, extraKeys, profiles, startup, theme, captureMethod, background,
             autoCopy, closeCopy, saveDir, format, quality, jpgBg, history, key, cache, days, limit] {
                if (key->keySequence().count() != 1) {
                    QMessageBox::warning(this, "快捷键无效", "请输入单组快捷键");
                    return;
                }
                if (!QColor(jpgBg->text()).isValid()) {
                    QMessageBox::warning(this, "颜色无效", "请输入有效的背景颜色");
                    return;
                }
                const auto candidate = config();
                if (candidate.backend == TranslationBackend::BaiduGeneral && !candidate.sessionKey.isEmpty()) {
                    QString error;
                    if (!SecretStore::store(SecretStore::baiduTranslationCredential(candidate.appId),
                                            candidate.sessionKey, &error)) {
                        QMessageBox::warning(this, "密钥未保存", error);
                        return;
                    }
                }
                if (startup->isChecked() != s.value("general/startup", false).toBool()) {
                    QString error;
                    if (!Autostart::setEnabled(startup->isChecked(), error)) {
                        QMessageBox::warning(this, "自动启动", error);
                        return;
                    }
                }
                s.set("general/startup", startup->isChecked());
                s.set("ai/profiles", QJsonDocument(*profiles).toJson(QJsonDocument::Compact));
                for (auto it = extraKeys->cbegin(); it != extraKeys->cend(); ++it)
                    s.set("hotkey/" + it.key(),
                          it.value()->keySequence().toString(QKeySequence::PortableText));
                runtime = candidate;
                s.saveProvider(runtime);
                s.set("general/theme", theme->currentIndex());
                s.set("capture/waylandMethod", captureMethod->currentData());
                s.set("general/background", background->isChecked());
                s.set("general/autoCopy", autoCopy->isChecked());
                s.set("general/closeAfterCopy", closeCopy->isChecked());
                s.set("general/saveDir", saveDir->text());
                s.set("general/format", format->currentText());
                s.set("general/quality", quality->value());
                s.set("general/jpegBackground", jpgBg->text());
                s.set("history/enabled", history->isChecked());
                s.set("hotkey/capture", key->keySequence().toString(QKeySequence::PortableText));
                s.set("ocr/engine", "paddleocr");
                s.set("ocr/cache", cache->isChecked());
                s.set("history/days", days->value());
                s.set("history/megabytes", limit->value());
                accept();
            });
}
} // namespace SnapLing

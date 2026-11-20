# 测试

## 自动化

```bash
./tools/build.sh
./tools/test-desktop.sh -o build/desktop-tests.log,txt
```

CTest 使用 offscreen Qt 平台，但执行真实图片算法、序列化与离线 OCR。包含负原点/多显示器坐标、125/150/200% DPI、三角/椭圆 Mask、裁剪旋转翻转、非破坏性撤销、PNG/JPG、工程损坏/未知版本、Unicode grapheme 跨行选择、OCR 坐标映射、JSON/块 ID 验证、背景和译文布局、导出、配置密钥不落盘、历史清理、本机 HTTP 异步/取消、实际中英文 OCR。

HTTP 测试服务器仅作为 **测试替身**，生产代码通过 QNetworkAccessManager 访问用户配置的真实服务。模型/服务响应验证和本地请求测试不能代替 Ollama 或远程实际模型验收。

安装了 `dbus-run-session` 时，CTest 还运行截图流程回归测试。测试在独立 D-Bus 会话中模拟 Screenshot Portal 响应，调用实际应用入口，验证系统区域/窗口截图直接打开编辑器、自定义形状进入选区、取消后重试以及编辑器退出时的窗口释放。这些响应来自测试替身，GNOME 系统截图授权仍需实机验收。

桌面测试在独立 Xvfb 1920×1080 根窗口上创建真实 QWidget 与 QMenu，再由生产 XCB 后端采集屏幕。校验菜单仍可见、菜单像素一致、区域选择、箭头编辑、PNG/JPG/工程保存重载、剪贴板、真实中英文 OCR、鼠标拖选中文及 Ctrl+C。使用 xdotool 发送实际 X11 按键，验证 XGrabKey 和冲突。25 次采集同时记录采集耗时、选区首帧耗时及 RSS 变化。测试图像不是用户桌面，不包含用户内容。

实际主程序端到端测试还可在独立 Xvfb 中运行 `python3 tools/test-application.py`，需要 Pillow 和 xdotool；它会启动单独的目标应用、通过 IPC 截图，并在 GUI 中标注和保存。

产物位于 `test-artifacts/`：`desktop-with-popup.png`、`desktop-edited.png/.jpg`、`desktop.snapling`、`editor-ui.png`。

Wayland 测试必须在真实登录会话中主动运行：

```bash
QT_QPA_PLATFORM=wayland build/snapling_portal_test
```

读取实际接口版本，并请求真实 Screenshot Portal。用户拒绝、权限失败或 10 秒内没有完成授权时，取消请求并明确标记 **SKIP**，不把空图片算作通过；不会自动开启持续屏幕共享，也不会保存测试时的用户桌面。

## 人工验收矩阵

| 环境 | 编译/自动化 | 必须补做的实机验收 |
|---|---|---|
| Ubuntu 26.04 / X11 | 本机编译；Xvfb 实际捕获已执行 | 原生 Xorg 会话、真实浏览器/GTK 菜单、双屏及混合 DPI |
| Ubuntu 26.04 / GNOME Wayland | Portal 接口探测和授权请求已执行；结果见执行记录 | Screenshot/快捷键授权、ScreenCast+PipeWire、恢复与断开 |
| Ubuntu 24.04 / X11 | 隔离容器构建与测试，结果见执行记录 | 真实登录会话、设备变化 |
| Ubuntu 24.04 / GNOME 46 Wayland | 需真实环境 | Portal、系统快捷键降级、共享停止 |
| Windows 10 | 源码与 CI 配置；未实际运行 | 编译、热键、右键菜单、UAC 降级、125/150/200% DPI |
| Windows 11 | 源码与 CI 配置；未实际运行 | 同上，显示器热插拔、运行库独立部署 |

每个平台还需验证：全屏/区域/窗口/自定义形状、Esc/Enter、撤销重做、OCR 中英文/选区复制、Ollama 实际翻译、远程服务超时/断网、译文导出、保存路径不可写/磁盘满、连续截图、模型不存在/损坏、长时间后台运行。

## 性能日志

主程序输出 `startup_ms`、`capture_ms`、`trigger_to_pixels_ms`、`trigger_to_selection_ms`、`image_export_ms`、`ocr_ms`（含 cold）和 `translation_ms`；无正文或密钥。选区时间在首个 paint 结束记录。原生后端采集时间与外部授权时间不能混为一谈。

建议使用 `/usr/bin/time -v` 记录峰值，读取 `/proc/<pid>/status` 的 VmRSS 记录真正空闲时的工作集；Windows 可使用任务管理器/Process Explorer。OCR 冷热测量使用同一实例连续识别两次，单独记录模型加载。持续共享需另测缓存、吞吐与 CPU，不纳入默认后台数据。

本次精确执行结果与产物清单见 [docs/VALIDATION.md](docs/VALIDATION.md)。CI 配置在 `.github/workflows/build.yml`，只有实际运行的 CI 日志才属于验证证据。

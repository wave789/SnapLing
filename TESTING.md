# 测试

## 自动化

```bash
./tools/build.sh
./tools/test-desktop.sh -o build/desktop-tests.log,txt
```

CTest 的核心套件使用 offscreen Qt 平台，执行真实图片算法、序列化与离线 OCR。包含负原点/多显示器坐标、125/150/200% DPI、三角/椭圆 Mask、裁剪旋转翻转、非破坏性撤销、PNG/JPG、工程损坏/未知版本、Unicode grapheme 跨行选择、OCR 坐标映射、JSON/块 ID 验证、背景和译文布局、导出、配置密钥不落盘、历史清理、本机 HTTP 异步/取消、实际中英文 OCR。

HTTP 测试服务器仅作为 **测试替身**，生产代码通过 QNetworkAccessManager 访问用户配置的真实服务。模型/服务响应验证和本地请求测试不能代替 Ollama 或远程实际模型验收。

`translation_screenshot` 重建深色/浅色菜单、后方文字、小字号路径，在 100%/150%/200% 像素比例下运行真实 Tesseract。验证窗口间文字分框、缩放和边距逆映射、路径/标题/同语言/低置信度过滤、字号范围、深浅底板对比色、文字框之外像素不变，以及 HTTP 翻译到 PNG 的完整链路。测试图由绘图代码生成，未使用用户原图；语义译文仍由测试服务器提供。产物为 `build/translation-menu-original.png` 和 `build/translation-menu-fixed.png`。

安装了 `dbus-run-session` 时，CTest 运行截图流程回归测试。在独立 D-Bus 会话中提供 ScreenCast Portal 测试替身和受控帧源，调用实际应用入口，验证整屏首帧、区域/窗口/自定义形状进入 SnapLing 选区、松开鼠标在同一遮罩内显示工具栏、Enter 后共享文档进入高级编辑。覆盖首帧延迟、临时共享关闭、主动持续共享、授权拒绝/取消/重试、旧响应隔离、负原点坐标和畸形节点列表。关闭 PipeWire 的构建验证非交互式 Screenshot 及拒绝后报错。这些测试未经过真实 PipeWire 视频传输或 GNOME 授权。

安装了 `xvfb-run` 和 `xdotool` 时，CTest 运行 `quick_edit`。外部目标进程提供实际窗口，生产 X11 热键和 XCB 后端捕获像素。xdotool 发送真实 X11 鼠标按下、移动、松开，确认工具栏在松开后属于原覆盖层，随后可画箭头；另验证缺少 MouseMove 事件的快速框选。Qt 鼠标/键盘事件覆盖即时箭头和矩形、中英文 OCR 跨行选择与复制、选区内翻译显示、PNG/JPEG 文件对话框保存与格式选择，以及 Enter 后对象修改和撤销重做。另验证 OCR/翻译未结束时进入高级编辑器、翻译失败的非阻塞提示和取消截图。翻译响应由本地 HTTP 测试替身提供。在 150% 和 200% 缩放下重跑真实鼠标框选、标注、OCR、高级编辑交接和译文保存。界面截图生成于 `build/quick-edit-ui.png`、`build/quick-edit-ui-150.png` 和 `build/quick-edit-ui-200.png`。

图标工具栏回归检查按钮图标、纯图标样式、提示、可访问名称和完整可见性，验证翻译按钮实际渲染的绿色像素、取消 OCR/网络任务后的原图与标注、忽略迟到响应、重新显示已完成图层不发出新请求，以及高级编辑器接管后的绿色状态。该流程也在 150%/200% 下执行。Xvfb 使用独立显示号，桌面套件由 CTest 串行执行以隔离共享宿主初始化，并保留 Xvfb 日志。

桌面测试在独立 Xvfb 1920×1080 根窗口上创建真实 QWidget 与 QMenu，再由生产 XCB 后端采集屏幕。校验菜单仍可见、菜单像素一致、区域选择、箭头编辑、PNG/JPG/工程保存重载、剪贴板、真实中英文 OCR、鼠标拖选中文及 Ctrl+C。使用 xdotool 发送实际 X11 按键，验证 XGrabKey 和冲突。25 次采集同时记录采集耗时、选区首帧耗时及 RSS 变化。测试图像不是用户桌面，不包含用户内容。

实际主程序端到端测试还可在独立 Xvfb 中运行 `python3 tools/test-application.py`，需要 Pillow 和 xdotool；它会启动单独的目标应用、通过 IPC 截图，并在 GUI 中标注和保存。

产物位于 `test-artifacts/`：`desktop-with-popup.png`、`desktop-edited.png/.jpg`、`desktop.snapling`、`editor-ui.png`。

Wayland 测试必须在真实登录会话中主动运行：

```bash
QT_QPA_PLATFORM=wayland build/snapling_portal_test
```

读取实际接口版本，默认请求真实 ScreenCast/PipeWire 整屏首帧，未启用 PipeWire 时请求非交互式 Screenshot Portal。用户拒绝、权限失败或 10 秒内没有完成授权时，取消请求并明确标记 **SKIP**。普通截图取得首帧后关闭临时共享，测试结束还会取消未完成请求；测试不会保存用户桌面。

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

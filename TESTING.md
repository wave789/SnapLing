# 测试

## 自动化

```bash
./tools/build.sh
./tools/test-desktop.sh -o build/desktop-tests.log,txt
```

CTest 的核心套件使用 offscreen Qt 平台，执行真实图片算法、序列化与离线 OCR。包含负原点/多显示器坐标、125/150/200% DPI、三角/椭圆 Mask、裁剪旋转翻转、非破坏性撤销、PNG/JPG、工程损坏/未知版本、Unicode grapheme 跨行选择、OCR 坐标映射、JSON/块 ID 验证、背景和译文布局、导出、配置密钥不落盘、历史清理、本机 HTTP 异步/取消、实际中英文 OCR。

HTTP 测试服务器仅作为 **测试替身**，生产代码通过 QNetworkAccessManager 访问用户配置的真实服务。模型/服务响应验证和本地请求测试不能代替 Ollama 或远程实际模型验收。

`translation_screenshot` 重建深色/浅色菜单、后方文字、小字号路径，在 100%/150%/200% 像素比例下运行真实 Tesseract。验证窗口间文字分框、缩放和边距逆映射、路径/标题/同语言/低置信度过滤、字号范围、深浅底板对比色、文字框之外像素不变，以及 HTTP 翻译到 PNG 的完整链路。测试图由绘图代码生成，未使用用户原图；语义译文仍由测试服务器提供。产物为 `build/translation-menu-original.png` 和 `build/translation-menu-fixed.png`。

安装了 `dbus-run-session` 时，CTest 运行截图流程回归测试。在独立 D-Bus 会话中提供 Screenshot/ScreenCast 测试服务和受控帧源，调用实际应用入口。默认单次截图验证三屏完整 PNG 拆分、负坐标、混合缩放、指定外接屏、没有 ScreenCast 调用、拒绝后不自动共享、截图布局不匹配、取消后的迟到响应以及单击整屏/拖动区域后原地显示工具栏。关闭 PipeWire 后仍运行单次多屏用例。兼容模式验证首帧延迟、取图关闭、保留会话时空闲暂停、只激活目标屏幕、重新请求新帧、拒绝/取消/重试及畸形节点。这些服务测试不能替代真实 GNOME 权限和真实 PipeWire 视频传输。

`portal_hotkey` 用私有 D-Bus 验证原生 QDBusObjectPath 会话解析、XDG keysym 格式、绑定成功检查、activation_token、旧会话隔离、取消/拒绝/关闭、空快捷键及请求超时。它不证明用户的物理快捷键已被 GNOME 允许。

截图流程另外模拟按唯一连接名缓存身份的 Registry：先在 Qt 共享连接复现空身份的 `code=2`、无 URI，再验证专用连接的 Register/Screenshot 顺序、单播响应的接收连接、共用拒绝和其他应用权限保持不变、连接复用、注册失败后换新连接重试、取消/销毁、服务重启重新注册及旧接口兼容。

还可用真实 xdg-desktop-portal 前端核对身份路由，需要系统 Python PyGObject 和 dbus-run-session：

```bash
/usr/bin/python3 tools/test-portal-identity.py --portal /usr/libexec/xdg-desktop-portal
```

工具会在私有 D-Bus、独立 XDG 目录中启动真实 Portal 前端。权限库、Access 和 Screenshot 后端使用测试替身。测试先复现缺少桌面入口的注册错误，再执行生产入口安装脚本。已注册的后台应用请求截图时，Access 按 GNOME 的焦点规则拒绝，前端返回 `code=2`，权限仍为 `unset`。随后测试通过生产 `Application` 触发截图，Access 在主窗口获得焦点后同意。截图后端按主窗口是否可见返回不同颜色的图片，测试验证编辑器只收到隐藏窗口后获取的图片。测试不会接触用户桌面。Portal 可执行文件路径按发行版调整。

另在隔离的 GNOME Shell/Mutter 48.7 Wayland 会话中运行了 SnapLing、Ubuntu 26.04 的 xdg-desktop-portal 1.21.1、GNOME 截图后端 48.0 和真实权限库。在其他应用拥有焦点时，已注册的后台截图请求返回 2，系统日志为 `Only the focused app is allowed to show a system access dialog`。新流程在主窗口获得焦点后显示 GNOME 授权弹窗。用鼠标点击允许，SnapLing 隐藏主窗口，取得三个 800×600 虚拟显示器的 2400×600 桌面图。在第三屏拖选后立即出现标注工具栏。再次触发时只请求一次截图。此桌面验证使用命令触发和鼠标操作，物理外接显示器与用户设备上的系统快捷键仍需实机验证。[系统授权弹窗](docs/validation/gnome-screenshot-consent.jpg) · [第三屏拖选后的工具栏](docs/validation/gnome-third-display-edit.jpg)

同一 GNOME 环境的三屏闪光对照测试使用 `--force-animations`，并在每个屏幕分别以 60 帧每秒采样像素。`org.gnome.desktop.interface enable-animations` 为 `true` 时三屏均出现白闪，为 `false` 时三屏均未出现白闪。两种设置下同一个单次截图入口均取得 2400×600 PNG 并显示三块选区覆盖层。GNOME 50 的截图后端同样请求 `flash=true`，Shell 的闪光采用遵循该动画设置的过渡。[GNOME 50 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/screenshotdialog.c#L247-L255) · [GNOME 50.1 动画设置](https://github.com/GNOME/gnome-shell/blob/50.1/js/misc/animationUtils.js#L13-L31)

在相同三屏环境中保持全局动画开启，对比了旧透明 CSS 和新版提速扩展。各运行三次，从启动 IPC 客户端触发已运行的 SnapLing 开始采样。首个选区可见的中位数从 701 毫秒降到 255 毫秒。以系统扩展路径加载时，在真实 GNOME 会话验证扩展初始未启用，从 SnapLing 设置页点击启用和停用后，系统状态和界面状态均正确更新。全局动画始终开启。实际构建并解包检查了 DEB 的扩展文件和目录权限。这些是 GNOME 48.7 虚拟三屏结果，未对物理三屏硬件承诺相同延迟。[扩展和计时说明](docs/GNOME_FLASH.md)

启动对照使用 Ubuntu 26.04 的 Qt 6.10.2 客户端和隔离 GNOME/Mutter 48.7。旧版从终端启动会留下两个未完成的启动通知，约 16 秒后光标才恢复。修复后终端启动无等待光标。使用 GNOME 应用菜单启动时，新版只提交一次已有的启动令牌，主窗口获得焦点，等待光标随启动完成而恢复。

OCR 回归覆盖旧路径失效、英文模型目录遮住中英文模型、`~/` 路径和首次自动复制随附模型。RapidOCR 在 Linux x86-64 的 Python 3.12 和 3.14 独立环境中完成真实模型安装和中英文识别。可在准备 RapidOCR 后运行 `QT_QPA_PLATFORM=offscreen SNAPLING_TEST_RAPIDOCR=1 build/snapling_tests realRapidOcrEngineSwitch`。用例通过设置窗口保存引擎选择，在同一编辑会话中执行 Tesseract → RapidOCR → Tesseract，验证识别文字和原图坐标。可用 `SNAPLING_OCR_HOME` 指向隔离的模型根目录。

额外设置 `SNAPLING_TEST_PADDLEOCR=1` 会加入真实 PP-OCRv6 Small 模型的切换和识别测试。另用旋转 180° 的中英文图片验证了上传的方向模型和对应配置。用户安装采用带空格的临时前缀和临时模型根目录，逐字节比较六个文件，验证再次安装保留已修改文件并补回缺项。DESTDIR 验证系统包带齐相同文件，安装后的辅助脚本可以从包内资源准备用户模型目录。

用户目录安装验证包含带空格的临时前缀：使用真实 Gio.DesktopAppInfo 读取生成的 desktop 文件，并通过该入口以 `--version` 启动已安装程序，检查绝对 Exec 路径、引号和安装 manifest。不要只用文件是否存在代替桌面入口有效性检查。

源码启动验证使用临时 XDG 目录：从没有入口的状态运行 `tools/run.sh --background --exit-after 200`，检查 `desktop_entry_installed=1`；通过真实 Gio 启动生成入口的 `--version`，检查失效入口修复、重复准备、带空格的源码路径，以及准备失败时不会继续启动 Qt。另检查 `--quit`/`--help`/`--version` 不写入口，其他应用入口不变，源码入口忽略打包 `DESTDIR`，正式安装仍保留正确的运行时 Exec 和 manifest。

多屏用例提供内置屏、HDMI、DP 三路不同颜色和原生尺寸的帧，故意打乱 Portal 节点顺序。验证 100%/150%/200% 混合缩放、负原点、同分辨率屏幕映射、指定外接屏只读取对应节点、等待较慢外接屏、授权不足后通过主界面重新选择、三处覆盖层中选定外接屏并继续标注/高级编辑、Esc 关闭所有遮罩后重试、全屏拼接包含各屏像素、旧单屏 token 迁移、持续共享切换目标、布局变化及会话关闭。核心套件验证拼接坐标、透明空隙、原生像素与尺寸上限。这些是合成帧和私有 D-Bus 的回归测试，不能替代真实笔记本与两台外接显示器的验证。

安装了 `xvfb-run` 和 `xdotool` 时，CTest 运行 `quick_edit`。外部目标进程提供实际窗口，生产 X11 热键和 XCB 后端捕获像素。xdotool 发送真实鼠标事件，验证悬停整屏、单击整屏、拖动区域和松开立即原地标注；另验证缺少 MouseMove 事件的快速框选。Qt 鼠标/键盘事件覆盖即时箭头和矩形、中英文 OCR 跨行选择与复制、选区内翻译显示、PNG/JPEG 保存，以及 Enter 后对象修改和撤销重做。另验证 OCR/翻译未结束时进入高级编辑器、翻译失败提示和取消截图。翻译响应由本地 HTTP 测试替身提供。在 150% 和 200% 下重跑整屏单击、鼠标框选、标注、OCR、高级编辑交接和译文保存。界面截图生成于 `build/quick-edit-ui.png`、`build/quick-edit-ui-150.png` 和 `build/quick-edit-ui-200.png`。

图标工具栏回归检查按钮图标、纯图标样式、提示、可访问名称和完整可见性，验证翻译按钮实际渲染的绿色像素、取消 OCR/网络任务后的原图与标注、忽略迟到响应、重新显示已完成图层不发出新请求，以及高级编辑器接管后的绿色状态。该流程也在 150%/200% 下执行。Xvfb 使用独立显示号，桌面套件由 CTest 串行执行以隔离共享宿主初始化，并保留 Xvfb 日志。

桌面测试在独立 Xvfb 1920×1080 根窗口上创建真实 QWidget 与 QMenu，再由生产 XCB 后端采集屏幕。校验菜单仍可见、菜单像素一致、区域选择、箭头编辑、PNG/JPG/工程保存重载、剪贴板、真实中英文 OCR、鼠标拖选中文及 Ctrl+C。使用 xdotool 发送实际 X11 按键，验证 XGrabKey 和冲突。25 次采集同时记录采集耗时、选区首帧耗时及 RSS 变化。测试图像不是用户桌面，不包含用户内容。

实际主程序端到端测试还可在独立 Xvfb 中运行 `python3 tools/test-application.py`，需要 Pillow 和 xdotool；它会启动单独的目标应用、通过 IPC 截图，并在 GUI 中标注和保存。

产物位于 `test-artifacts/`：`desktop-with-popup.png`、`desktop-edited.png/.jpg`、`desktop.snapling`、`editor-ui.png`。

Wayland 测试必须在真实登录会话中主动运行：

```bash
QT_QPA_PLATFORM=wayland build/snapling_portal_test
```

读取实际接口版本，随后请求真实非交互式 Screenshot Portal。拒绝、权限失败或 10 秒内没有完成授权时，取消请求并明确标记 **SKIP**；测试不会保存用户桌面。

在已有至少三个输出的 Weston/Wayland 会话中可运行覆盖层测试：

```bash
SNAPLING_TEST_WAYLAND=1 QT_QPA_PLATFORM=wayland dbus-run-session -- \
  build/snapling_capture_flow_test waylandOutputsExposeFrozenOverlays
```

此用例使用真实 Wayland surface 和 QScreen，检查各窗口 isExposed、全屏尺寸、绿色边框和单击后即时工具栏。PNG 来自明确的测试服务。默认点击由 QtTest 发送；在 Weston X11 后端上，可设置 `SNAPLING_TEST_WAYLAND_X11_WINDOWS` 为按 QScreen 顺序排列的三个父窗口 ID，并提供对应 DISPLAY/XAUTHORITY，通过 xdotool → Weston seat → Wayland 的真实输入通道验证逐屏悬停、只有当前屏幕高亮、单击第三屏、再次截图后在第三屏拖选及像素尺寸。父窗口应按输出布局分开放置。Qt 的推算 screen() 属性可能变化，不能单独作为窗口实际放错屏的结论。此测试不验证 GNOME 的实际 Screenshot 授权或三屏视频。

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

主程序输出 `startup_ms`、`capture_backend`、`capture_ms`、`trigger_to_pixels_ms`、`trigger_to_selection_ms`、`hotkey_ready`、`image_export_ms`、`ocr_ms` 和 `translation_ms`；无正文、密钥或 activation token。选区时间在首个 paint 结束记录，包含屏幕名称；paint 本身不证明合成器显示了窗口，Wayland 专项测试另检查 isExposed。采集和外部授权耗时不能混为一谈。

建议使用 `/usr/bin/time -v` 记录峰值，读取 `/proc/<pid>/status` 的 VmRSS 记录真正空闲时的工作集；Windows 可使用任务管理器/Process Explorer。OCR 冷热测量使用同一实例连续识别两次，单独记录模型加载。持续共享需另测缓存、吞吐与 CPU，不纳入默认后台数据。

本次精确执行结果与产物清单见 [docs/VALIDATION.md](docs/VALIDATION.md)。CI 配置在 `.github/workflows/build.yml`，只有实际运行的 CI 日志才属于验证证据。

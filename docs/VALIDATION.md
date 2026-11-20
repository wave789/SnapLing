# 实际执行记录

2026-11-20 更新：默认捕获路径已改为单次桌面截图，最新验证追加在文末；前面的 ScreenCast 默认流程记录对应历史版本。

执行日期：2026-11-19，Asia/Shanghai。以下数据来自实际命令和日志；没有将配置文件或编译成功替代实机验证。

## 构建与测试结果

| 项目 | 实际结果 | 证据 |
|---|---|---|
| Ubuntu 26.04.1 x86_64 / GCC 15.2 / Qt 6.10.2 | CMake + Ninja 成功，含 Tesseract、XCB/RandR、Portal、PipeWire、ZXing | `build/configure.log`、`build/compile.log` |
| 核心自动化，26.04 | QtTest 23 项通过，0 失败、0 跳过 | `build/core-tests.log` |
| 真实 X11 桌面集成，26.04 | QtTest 5 项通过，0 失败 | `build/desktop-tests.log` |
| Ubuntu 24.04 容器 / GCC 13.3 / Qt 6.4.2 | 实际编译成功，核心 23 项通过，桌面 5 项通过 | `build-ubuntu24/container.log`、`build-ubuntu24/build/Testing/Temporary/LastTest.log`、`build-ubuntu24/desktop-tests.log` |
| 单实例主程序 IPC | 实际启动进程，经第二实例投递退出命令成功 | `build/ipc-tests.log` |
| 26.04 便携包搬移 | 清除开发 LD_LIBRARY_PATH / QT_PLUGIN_PATH / TESSDATA_PREFIX，搬到临时目录，GUI 启动并触发真实 OCR 成功 | `build/portable-tests.log` |
| 24.04 基线包在 26.04 搬移运行 | 同样清除开发路径，Qt 6.4 GUI 与离线 OCR 成功 | `build/portable-ubuntu24-on26-tests.log` |
| 模型校验 | 两个 bundled traineddata 的 SHA256 均匹配 | `build/model-checks.log` |
| DEB 安装，24.04 | 在隔离容器实际 dpkg 安装成功，安装后的 `snapling --version` 输出 0.1.0 | `build-ubuntu24/install-tests.log` |
| GNOME Wayland | 实际探测 Screenshot v2 / ScreenCast v5 / GlobalShortcuts v1；普通请求拒绝，交互式重试未获得图像。截图测试明确 SKIP | `build/portal-tests.log` |
| Windows 10 / 11 | 未执行编译或 GUI 测试；没有可用的已配置远程仓库来派发 Windows CI | 只有源码与 `.github/workflows/build.yml`，不作为通过证据 |

QtTest 的统计数包含初始化和清理；桌面套件包含三个主体场景，内部逐项校验截图/编辑/OCR/复制、热键/冲突、连续截图/内存。

## 完整实际工作流程

Xvfb 1920×1080 的真实根窗口上创建普通应用窗口与 QMenu。生产 XCB 后端采集实际桌面，验证菜单仍然可见且像素与菜单窗口一致。通过选区控件拖动裁切，添加箭头，保存 PNG/JPG/工程并重载，校验剪贴板。

对同一张真实 1080P 桌面捕获图运行中英文 OCR，映射行框回裁切后的图片，鼠标拖选含「中文」的文字行，按 Ctrl+C，验证系统剪贴板 Unicode 内容正确。

可检查 [编辑结果](../test-artifacts/desktop-edited.png)、[保留菜单的桌面](../test-artifacts/desktop-with-popup.png)、[OCR 选择界面](../test-artifacts/editor-ui.png)。这些是独立测试桌面产生的图像，不是用户桌面。

翻译测试调用实际 QNetworkAccessManager 和本机 HTTP 测试服务器，验证请求、块 ID、返回解析、取消和超时；另外验证中文排版、复杂背景底板、实际译文 PNG 像素、溢出时拒绝导出。**没有把测试服务器当成真实 Ollama/远程大模型验收**。当前机器没有运行的 Ollama 与可用的远程模型凭据，因此真实模型的语义质量和服务往返尚未测试。

另已运行 `tools/test-application.py`：由独立目标进程显示窗口和菜单，经真实主程序 IPC 全屏截图、GUI 绘制箭头和 GUI 文件对话框保存 PNG；校验 1920×1080 尺寸与 1,449 个红色标注像素。结果 `build/application-tests.log` 为 PASS，导出文件 [application-ui-saved.png](../test-artifacts/application-ui-saved.png) 约 46 KiB。测试进程使用 compose 输入上下文和临时目录隔离中文输入法及保存路径。

## 性能实测

| 项目 | Ubuntu 26.04 / Qt 6.10.2 | Ubuntu 24.04 容器 / Qt 6.4.2 |
|---|---:|---:|
| 主程序后台初始化 | 34 ms | 未单独测量 |
| 未加载 OCR 的空闲 RSS | 51,956 KiB（约 50.7 MiB） | 未单独测量 |
| 1080P 原生采集 P95，25 次 | 30 ms | 31 ms |
| 采集请求至冻结选区首帧 P95，25 次 | 46 ms | 45 ms |
| 25 次采集及选区显示 RSS 变化 | 147,840 → 148,412 KiB | 155,256 → 155,916 KiB |
| 真实 1920×1080 测试桌面 OCR 冷启动 | 190 ms | 197 ms |
| 同图 OCR 热启动 | 78 ms | 80 ms |
| 765×475 编辑后 PNG/JPG 保存 | 15 / 8 ms | 17 / 6 ms |
| 1000×240 混合文字测试图 OCR 冷/热 | 204 / 42 ms | 155 / 36 ms |

桌面是包含中英文和菜单的稀疏测试布局，不代表密集文档、竖排、4K 或真实多屏性能。P95 起点为后端采集请求，包含异步调度与选区绘制；没有把它宣称为物理键盘到显示器扫描输出的端到端延迟。连续截图的 RSS 在先前 OCR 与编辑流程后测量，不能与「未加载模型的空闲 RSS」混用。

曾在 Qt 6.4 上发现 25 次截图后 RSS 增长超过 200 MiB。保持原有严格内存测试后，将采集改为复用专用单线程池，增长降到约 0.6 MiB；不是通过取消测试或调高阈值通过。

GTK 平台主题曾间接加载 Mesa/LLVM，使后台 RSS 超过 100 MiB。默认使用 Qt generic 光栅主题后达到上述空闲值；可由用户环境变量覆盖。

## 交付物

- `build/snapling`：含调试符号的当前开发程序；使用 `tools/run.sh` 加载本地开发依赖。
- `dist/SnapLing/snapling`：Ubuntu 26.04 Qt 6.10 便携启动器。
- `dist/SnapLing-ubuntu24/snapling`：Ubuntu 24.04 Qt 6.4 基线便携启动器，已在当前 26.04 运行 OCR。
- `dist/snapling_0.1.0_ubuntu24.04_amd64.deb`：自动分析依赖、实际安装验证的 24.04 包。
- `dist/snapling_0.1.0_ubuntu26.04_amd64.deb`：26.04 包；未直接安装到用户主系统。
- `dist/SnapLing-0.1.0-ubuntu24-x86_64.tar.gz`、`dist/SnapLing-0.1.0-linux-x86_64.tar.gz`：对应便携目录归档。
- `dist/SHA256SUMS`：最终包校验。

便携二进制的 RUNPATH 为 `$ORIGIN/../lib`。运行时不需要源码、`.deps`、Python、Node 或 `/tmp` 开发路径。Qt 及依赖动态库保留可替换性，许可证在 `share/licenses`。

开发目录未安装 desktop 文件时，Qt 6.10 可能记录「App info not found」的 Portal 注册提示；已打包 desktop 文件。该提示不影响已验证的 X11 工作流，但 Wayland 必须通过正常安装后的系统授权继续验收。

AppImage、Windows 安装包、GNOME 授权后截图/快速共享、物理双屏/混合 DPI/4K、长时间稳定性和实际模型服务仍未完成，详见 [LIMITATIONS.md](../LIMITATIONS.md)。优先后续工作为：Wayland 实机授权回归、Windows CI 与实机、共享 OCR 模型缓存、WGC、复杂背景修复。


## 即时截图编辑改造，2026-11-20

本次实际验证机为云端 Debian 13 x86_64、GCC 14.2、Qt 6.8.2。截图交互使用独立 Xvfb 桌面，Portal 响应和翻译响应分别由独立 D-Bus 会话及本地 HTTP 测试替身提供。

| 项目 | 结果 |
|---|---|
| CMake/Ninja 完整构建 | 通过 |
| 核心 QtTest | 38 通过，0 失败、0 跳过 |
| Portal 截图阶段与共享文档交接 | 11 通过，0 失败、0 跳过 |
| 即时编辑真实 X11 交互 | 11 通过，0 失败、0 跳过 |
| 150% 缩放交互 | 6 通过，0 失败、0 跳过 |
| 200% 缩放交互 | 6 通过，0 失败、0 跳过 |
| 原有 X11 桌面集成 | 5 通过，含菜单捕获、编辑保存、实际热键与冲突、连续截图内存检查 |
| 主程序 IPC 截图、Enter 交接、GUI 标注、GUI 保存 | 通过，PNG 1920×1080，1438 个红色标注像素 |

QtTest 统计包含初始化与清理。5 组 CTest 合计 72 项，结果位于 `build/Testing/Temporary/LastTest.log`。界面截图位于 `build/quick-edit-ui.png` 及对应缩放后缀的文件。

实际交互覆盖框选后直接画箭头和矩形、中英文 OCR 跨行与全选复制、选区内翻译显示、译文/图片复制、PNG/JPEG 文件对话框保存与格式选择、文字输入的 Enter 优先级、进入高级编辑器后移动与修改已有对象及撤销重做。另验证 OCR/翻译未结束时进入高级编辑器，以及翻译失败、取消截图和带未完成请求退出应用的情况。

该轮未实机验证 Ubuntu 26.04.1 GNOME Wayland 的系统授权、窗口激活、物理多屏和混合 DPI，以及真实 Ollama/OpenAI 模型服务。当时普通 Screenshot Portal 可能弹出系统界面并改变焦点，返回的已选区域按比例居中显示。后续入口修正见下一节。

## Wayland 松开即标注入口修正，2026-11-20

修正 Ubuntu 系统截图选区要求额外确认的入口。默认 Wayland 截图通过 ScreenCast/PipeWire 取得完整显示器首帧，关闭本次临时共享，然后打开 SnapLing 自身选区。拖动松开时，在原遮罩内创建 Canvas 和浮动工具栏；Enter 或 ✅ 交给独立高级编辑器。鼠标释放坐标用于完成选区，支持未收到最后一次 MouseMove 的快速拖动。

验证环境为 Debian 13 x86_64、GCC 14.2、Qt 6.8.2。

| 项目 | 结果 |
|---|---|
| 默认 PipeWire 构建 | 通过 |
| 核心 QtTest | 38 通过 |
| ScreenCast 调用链与阶段交接 | 15 通过 |
| 即时编辑 X11 交互 | 13 通过 |
| 150% 缩放交互 | 7 通过 |
| 200% 缩放交互 | 7 通过 |
| 关闭 PipeWire 的独立构建与截图流程 | 构建通过，9 通过 |

默认 5 组 CTest 合计 80 项，统计含初始化与清理，全部通过且无跳过。首次并行执行出现 xvfb-run 空闲显示号争用，已为三组 Xvfb 测试加入 CTest 资源锁和超时，重跑通过。完整日志位于 `build/instant-release-ctest.log`，截图流程复查位于 `build/instant-release-capture-flow.log`，兼容构建记录位于 `build/instant-release-no-pipewire.log`。

新增 Xvfb 用例使用 xdotool 发送实际 X11 鼠标按下、移动、松开事件，验证释放前尚在框选、释放后工具栏可见、覆盖层窗口 ID 不变、工具栏属于原覆盖层，并立即画箭头。另覆盖省略 MouseMove 的释放事件。原有 OCR、翻译、保存和高级编辑交接继续通过。

ScreenCast 测试使用独立 D-Bus 会话的 Portal 替身和受控帧源，覆盖完整首帧等待、单显示器授权、有效 FD 传递、临时会话关闭、持续共享复用、拒绝后重试、取消后重试、旧授权响应隔离、负原点元数据和错误节点列表。关闭 PipeWire 时验证只请求非交互式 Screenshot，拒绝后显示错误。

本轮未执行真实 GNOME 系统授权和 PipeWire 视频帧传输，也未实机验证物理多屏、混合 DPI、窗口焦点及真实 Ollama/OpenAI 模型。系统是否再次请求屏幕访问授权由桌面决定，自动化通过不代表这些场景已完成验收。

## 原地翻译排版与图标工具栏，2026-11-20

检查用户提供的深色 IDE 菜单截图后，用 Qt 重建相似的多窗口、小字号路径测试图。原 PSM_AUTO 把菜单项目名和右侧背景内容合成了宽达 800 多像素的文本行，译文自动扩宽和独立计算底色/文字色又扩大了遮挡。改为分散文本识别、词间距分框、深色预处理、小字号放大和像素坐标逆映射。新译文以原文字高度估计字号，只允许小幅缩小，并在原文字框内绘制；路径、项目标题、日志键值、已有目标语言和识别存疑内容自动保留。

工具栏使用原创矢量图标和深色样式，名称保留在工具提示与可访问属性中，样式选项放入弹出面板。翻译按钮在 OCR、请求和显示译文时变绿，再次点击取消未完成任务并显示原图，保留标注和已有译文图层。已完成图层可重新显示，进入高级编辑器后状态继续同步。

本轮环境为 Debian 13 x86_64、GCC 14.2、Qt 6.8.2。

| 项目 | 结果 |
|---|---|
| CMake/Ninja Release 构建 | 通过 |
| 核心 QtTest | 38 通过 |
| ScreenCast 调用链 | 15 通过 |
| 深浅色、多列、小字号及译文 PNG | 13 通过 |
| 即时编辑 X11 交互 | 15 通过 |
| 150% 缩放交互 | 8 通过 |
| 200% 缩放交互 | 8 通过 |

完整 6 组 CTest 合计 97 项，包含初始化与清理，全部通过且无跳过。日志位于 `build/translation-and-toolbar-ctest.log`。末尾确认按钮曾在任务切换后被挤进工具栏折叠菜单，已计入边框/边距并限制子工具栏最小宽度，回归检查全部主要按钮可见和未完成 OCR 时进入高级编辑器。并行测试曾出现 Xvfb 连接失败，现隔离桌面套件为串行执行并保留服务器日志，完整重跑通过。

回归覆盖实际绿色按钮像素、取消 OCR、取消延迟 HTTP 请求、忽略迟到响应、恢复原图时保留箭头、再次显示译文不重复请求、高级编辑器接管和按钮同步。最后的连续点击状态与图标复查记录位于 `build/translation-toolbar-final.log`。工具栏截图为 `build/icon-toolbar.png`，完整界面截图为 `build/quick-edit-ui.png` 及缩放版本。合成菜单原图与译文图为 `build/translation-menu-original.png`、`build/translation-menu-fixed.png`，逐像素验证文字框之外保持不变。

菜单图由测试代码绘制，未使用用户原图像素；OCR 使用真实 Tesseract，译文由本机 HTTP 测试服务器提供。未验证用户本机 Ubuntu GNOME Wayland 的真实交互、真实 Ollama/OpenAI 模型语义质量，也无法保证恢复原字体族或修复所有低质量 OCR。

## 笔记本与两台外接屏，2026-11-20

原 Wayland 后端忽略 `screen` 参数，只读取 Portal 返回的第一路流，恢复授权还可能持续选择内置屏。本轮改为请求多选显示器、解析全部节点、按屏幕逻辑位置/尺寸匹配 Qt 输出，尊重指定显示器，并为每路独立打开 PipeWire 连接。多屏授权单独保存恢复 token，旧单屏 token 不再复用；主界面提供「重新选择共享屏幕…」。

区域模式在各输出创建独立覆盖层，在任一屏松开鼠标后关闭其他遮罩，继续该屏的即时编辑和同一高级编辑文档。单屏区域保持原生像素，全屏桌面在工作线程统一像素密度拼接并按比例预览；支持负原点、透明空隙和 1 亿像素上限。部分授权、元数据不足和显示器变化会明确报告。

环境为 Debian 13 x86_64、GCC 14.2、Qt 6.8.2。

| 项目 | 结果 |
|---|---|
| 默认 PipeWire Release 构建 | 通过 |
| 核心 QtTest，含多屏拼接与尺寸限制 | 41 通过 |
| Portal/多屏/覆盖层交接 | 29 通过 |
| 翻译截图回归 | 13 通过 |
| 即时编辑 X11 交互 | 15 通过 |
| 150% 与 200% 缩放 | 各 8 通过 |
| 关闭 PipeWire 的独立构建与截图流程 | 构建通过，10 通过 |

完整 6 组 CTest 合计 114 项，统计含初始化与清理，全部通过且无跳过。日志位于 `build/multiscreen-ctest.log`；未启用 PipeWire 的记录为 `build/multiscreen-no-pipewire.log`。

三屏回归使用私有 D-Bus Portal 与受控帧源，模拟内置屏、HDMI、DP，缩放分别为 100%、150%、200%，将节点顺序打乱。验证选择两台外接屏时均读取正确的原生图像、同分辨率屏幕依靠位置区分、等待外接屏首帧、各流独立打开与停止、授权不足后通过主界面重新选择、外接屏区域继续画箭头并保留到高级编辑器、Esc 取消所有覆盖层后重试、全屏图包含三屏像素、旧 token 迁移、持续共享切换屏幕、布局变化重新授权、旧会话关闭信号不影响当前会话。兼容构建验证多屏时明确要求 PipeWire，单屏继续使用非交互式 Screenshot。

没有执行真实 GNOME 授权、PipeWire 视频传输和三个物理输出上的 Wayland 全屏 surface 验收。当前区域选区限于一块显示器内，尚不支持跨屏连续拖动。物理双外接屏、混合 DPI、热插拔和全屏窗口放置仍需在用户设备上验证。

## 2026-11-20：单次桌面与即时整屏选择

本轮环境为 Debian 13、GCC 14.2、Qt 6.8.2；不是用户的 Ubuntu 26.04.1 登录桌面。完成默认 Screenshot(interactive=false) 路径、完整桌面拆分、悬停整屏/单击整屏/拖动区域、Wayland 普通全屏窗口、单次激活及 token 转发、快捷键会话解码和异步版本查询。正常界面不再提供持续共享入口；兼容流在无请求时暂停，每次只复制首帧。

| 验证 | 结果 |
|---|---|
| Release 构建（含 PipeWire、Tesseract） | 通过 |
| 核心 / 翻译截图 / 截图流程 / Portal 快捷键 | 41 / 13 / 36 / 7 通过；常规 minimal 平台跳过 1 项真实 Wayland 条件用例 |
| 即时编辑 / 150% / 200% | 16 / 9 / 9 通过 |
| 完整 CTest | 7 组全部通过，QtTest 合计 131 通过、1 按条件跳过、0 失败 |
| 禁用 PipeWire 的独立构建 | 通过；截图流程 17 通过、1 条件跳过，仍覆盖单次三屏 |
| 原生 Wayland 三输出鼠标 | Weston 14.0.2 / Pixman / X11 后端 / Xvfb 3000×1200；3 个 800×600 输出。xdotool → Weston seat → Wayland 客户端，逐屏悬停且其他屏不高亮、第三屏单击选整屏、再次截图后在第三屏拖选 220×170、释放原地显示工具栏均通过 |
| 单实例及启动 token 转发 | 主程序实际启动与命令往返通过，测试 token 不入日志 |

最终 CTest 用时 142.63 秒。首次全量运行的 150% 组曾因 OCR 超过测试 10 秒等待上限失败（实际约 21 秒），单独复测和最终完整运行均通过；没有为此修改生产 OCR 或放宽该测试上限。

三输出测试使用真实 Wayland surface 和原生输入，但图像来自私有 D-Bus Screenshot 测试服务，不能替代系统取图授权。测试通过逐屏鼠标命中、可见绿色边框和裁切尺寸验收；本环境的 Qt 推算 screen() 属性会变化，不能仅据该属性判断窗口实际位置。未计入没有输入设备的 headless 合成器试验或早期只在首次 paint 后立即检查属性的结果。

可保留审阅的日志摘要见 [本轮记录](validation/2026-11-20-instant-capture.txt)。本地完整日志为 `build/instant-capture-final-ctest.log`、`build/wayland-three-output-native.log`；独立兼容构建记录在 `/tmp/snapling-no-pipewire/Testing/Temporary/LastTest.log`。

**未验证：**真实 GNOME 的单次截图权限存储和 PNG 返回、用户笔记本加两块外接屏同时播放视频、物理鼠标与快捷键、实际混合 DPI、系统 Portal NoReply 的根本原因和长时间性能。不能宣称已在用户的 Ubuntu 26.04.1 三屏环境完成验收。首次系统截图授权仍可能出现；取图有系统 API 延迟。当前区域仍限于一块屏幕内。

### 后续：截图失败无提示

用户反馈快捷键已注册、重复输出 `capture_backend=screenshot`，没有图像完成记录，且启动时鼠标转圈约 15 秒后没有可见反馈。该日志不足以判断权限、服务、文件读取或布局匹配的具体原因；没有把它认定为已解决的系统权限问题。

增加 Screenshot Portal 响应码、是否返回 URI、PNG 尺寸、映射桌面尺寸和失败原因日志（不记录图像路径或内容）。错误置于主界面顶部并突出显示；存在托盘时发送失败通知。主窗口恢复最小化状态，命令启动及失败回到主界面也传递激活 token。测试覆盖 Portal 取消、通用失败、布局不匹配、最小化窗口恢复、无隐式共享和失败后重试成功。Release 构建、capture_flow 和 portal_hotkey 回归通过；用户实际失败的原因仍需新的日志确认。

### 后续：Portal 返回 2 的权限恢复

用户的新日志确认 Screenshot 返回 `code=2 has_uri=0`，未得到图片。对照 xdg-desktop-portal 1.20.3 上游，非交互截图遇到保存的 `no` 会直接返回 2；其他服务错误也可返回此码，尚不能确认用户设备的具体原因。

新增失败后的异步 PermissionStore Lookup，区分本应用拒绝、未识别主机应用拒绝、没有这两类拒绝和查询失败。新增主界面「重新请求截图授权」：用户点击时只对 `screenshot/screenshot` 中 `io.snapling.SnapLing` 的权限执行 SetPermission 空列表，触发下一次由系统询问；不写 `yes`，不改空应用 ID 或其他应用，不调用交互式系统框选或 ScreenCast。Qt 平台初始化前设置 desktopFileName，启动时报告 desktop 身份文件是否存在，并补充用户目录安装说明。

私有 D-Bus 测试覆盖已记住的拒绝、未识别主机拒绝、允许状态下的后端失败、从主界面重新请求后截图成功、其他应用及共享记录保持不变、权限写入被拒绝后不开始截图。测试服务提供权限库和截图响应，未在用户 GNOME 设备实际修改权限。

Release 构建通过；截图流程 45 通过、1 个真实 Wayland 条件用例跳过；Portal 快捷键 7 通过。禁用 PipeWire 的独立构建及截图回归通过。用户目录安装路径在 `/tmp` 前缀执行安装并启动 `--version` 成功，主程序单实例命令往返通过。未将这些结果当作用户设备已恢复截图的证据。

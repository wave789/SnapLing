# 实际执行记录

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

Ubuntu 26.04.1 GNOME Wayland 的系统授权、窗口激活、物理多屏和混合 DPI，以及真实 Ollama/OpenAI 模型服务尚未在本次云端实机验证。普通 Screenshot Portal 可能弹出系统界面并改变焦点。已选区域缺少原屏幕位置时，覆盖层按比例居中显示返回图像。主动授权快速共享路径可从缓存的全屏帧进入选区。

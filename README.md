# SnapLing

C++20 / Qt6 Widgets 截图工具。提供真实桌面捕获、非破坏性标注、图片上的离线 OCR 文字选择，以及兼容 Ollama 和 OpenAI Chat Completions 的原地翻译。

当前版本为 **0.1.0**。本仓库包含可执行实现、离线中英文模型、自动化测试和打包工具。Windows 与 Wayland 快速共享仍需要交互式实机验收，不能把此版本视为所有平台均已完成验收的正式发布版。具体执行记录见 [TESTING.md](TESTING.md)，缺项见 [LIMITATIONS.md](LIMITATIONS.md)。

## 已实现

- Windows GDI / Linux X11 XCB+RandR / Wayland Screenshot Portal 截图后端；在显示选区前采集桌面，保留可捕获的普通菜单。
- 主屏幕、指定显示器、整个桌面；矩形、窗口边界选择、椭圆、多边形、自由曲线；延时及倒计时取消。Wayland 显示器和窗口由系统选择器决定。
- 原生或 Portal 全局快捷键、冲突提示、单实例命令转发、托盘和独立启动器入口。
- 矩形、椭圆、直线、箭头、画笔、荧光笔、文字、序号、马赛克、近似模糊、裁剪、旋转、翻转；标注颜色、线宽、字体、透明度、移动、修改和删除；撤销/重做。
- `.snapling` 版本化工程保存原图、编辑指令、OCR 和翻译布局；PNG 透明导出、JPG 背景色；异步保存图片。
- 按需初始化的 Tesseract 5 LSTM 本地 OCR，中文/英文/混合；直接在图像上拖选、Unicode 复制、全选、逐行选择、TXT/Markdown 导出。**目前没有接入 RapidOCR/ONNX Runtime**。
- Ollama / OpenAI 兼容 HTTP API：供应商配置、环境变量密钥、连接测试、异步翻译、取消、超时、ID 校验、原地译文渲染和图片导出。
- 简单背景估色覆盖、复杂背景底板、字号自适应、手动调整译文；容纳不了的文字阻止图片导出，工程仍保留完整内容。
- 贴图、HEX 取色、ZXing 二维码识别、需确认的敏感信息打码建议、可关闭历史、主题与用户登录自动启动。
- 用户主动开启的 ScreenCast Portal / PipeWire 快速截图实现：状态提示、停止共享、restore token、单帧缓存；尚未完成授权后的实机验收。

## 构建和运行

Ubuntu 24.04 / 26.04：

```bash
sudo apt install build-essential cmake ninja-build pkg-config libxkbcommon-dev qt6-base-dev qt6-wayland qt6-svg-dev \
  libtesseract-dev libpipewire-0.3-dev libx11-dev libxrandr-dev libxcb1-dev \
  libzxing-dev xvfb xauth xdotool fonts-noto-cjk
./tools/build.sh
./tools/run.sh
```

没有 sudo 的开发机可执行 `python3 tools/bootstrap-local.py`，将经 apt SHA256 校验的依赖提取至 `.deps`。这仅是开发工具，主程序不依赖 Python。详细配置与 Windows 构建见 [BUILD.md](BUILD.md)。

当前开发环境的可运行入口：`build/snapling`；项目内依赖环境使用 `tools/run.sh`。便携包入口为 `dist/SnapLing/snapling`，不需要 `.deps`。

```bash
snapling --capture        # 矩形选区
snapling --full           # 全屏，显示器可在主界面选择
snapling --window         # 窗口边界选区；Wayland 使用系统选择器
snapling --ocr
snapling --translate
snapling --delay 3
snapling --open picture.png
snapling --clipboard
snapling --background
snapling --quit
```

重复启动会把命令传给已有实例。默认 **Ctrl+Alt+A**；全屏、窗口、延时、OCR、翻译的额外快捷键可在设置中配置。系统不提供 GlobalShortcuts Portal 时，在 GNOME「设置 → 键盘 → 自定义快捷键」绑定 `snapling --capture`。GNOME 没有托盘图标时，重新从启动器打开即可访问主界面。

## 截图与编辑

1. 后台按快捷键，或从主界面选择模式。截图前不会先显示 SnapLing 选区窗口。
2. 拖动框选；选区内拖动可移动，右下角可调整，方向键微移，Shift+方向键改变尺寸，Alt 暂停边界吸附。
3. Enter / 双击进入编辑，Esc / 右键取消；多边形逐点单击，双击闭合。
4. 编辑器选择工具后绘制；「选择」工具移动标注，Delete 删除；双击文字修改；Ctrl+Z / Ctrl+Shift+Z 撤销/重做。
5. Ctrl+C 在 OCR 选中文字时复制文字，其余情况复制图片；保存图片或可编辑工程。

Wayland 系统截图工具完成区域或窗口截图后，SnapLing 直接打开编辑器。椭圆、多边形和自由曲线在冻结图像上完成形状选择。

## 离线 OCR

模型随源码提供，约 6.4 MiB，安装至 `share/snapling/tessdata`，无需联网下载。首次点击 OCR 才加载模型。手动导入时选择含 `eng.traineddata` / `chi_sim.traineddata` 的目录。来源、许可证、大小和 SHA256 见 [models/README.md](models/README.md)。

识别后直接拖动图中文字；双击选择连续字词，Ctrl+双击选择整行。「选全部文字」「复制全部文字」「导出文字」可保留换行。行框来自引擎，字符位置依据字宽估算，不代表精准字符检测。OCR 高亮不会写入导出的图片。

## AI 翻译配置

默认本机 Ollama：Base URL `http://127.0.0.1:11434/v1`，模型 `qwen3.5:4b`。模型须由用户在 Ollama 中预先安装；SnapLing 不启动、下载或常驻加载大模型。

设置 → AI 翻译：配置地址、模型、目标语言、超时、temperature、token 上限，可添加/更新/删除供应商。支持兼容 `/chat/completions` 的服务，连接测试使用 `/models`。不支持 JSON 模式的服务可关闭 `response_format`，但仍需返回可解析的块 ID JSON。

本机服务默认开启「关闭思考模式」，通过 `reasoning_effort: none` 避免 Qwen 等模型在生成译文前耗尽输出 token。兼容服务不支持此参数时可取消勾选。生成被截断、仅返回思考过程或返回空内容时，应用会显示对应原因。

密钥使用环境变量 `SNAPLING_API_KEY`（可改变量名），或本次运行有效的临时密钥。密钥不写入配置和日志。远程服务发送前明确显示目标地址并确认；本机服务也明确显示地址。只有主动翻译会发送 OCR 文字，不发送截图图片，不提供工具执行能力。

点击「原地翻译」后切换原文/译文/对照，可调整译文、字号、区域高度、背景底板。保存 PNG/JPG 与复制图片会包含实际译文像素。「保存工程」保留可再次修改的数据。

## 隐私与部署

历史默认关闭，仅启用后在图片导出时写入历史。OCR 和翻译不会自动运行；搜索、网络翻译、屏幕共享均须主动触发。工程保留原始图像，**打码后的工程仍含未打码原图**，对外分享请导出 PNG/JPG。

包构建：`tools/package-linux.sh`；AppImage：`packaging/linux/appimage.sh`；Windows 部署与安装脚本位于 `packaging/windows`。不同 Ubuntu 版本应在对应发行版或更老基线上构建，不能将 Ubuntu 26.04 编译的程序直接宣称兼容 24.04。

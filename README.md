# SnapLing

C++20 / Qt6 Widgets 截图工具。提供真实桌面捕获、非破坏性标注、图片上的离线 OCR 文字选择，以及兼容 Ollama 和 OpenAI Chat Completions 的原地翻译。

当前版本为 **0.1.0**。本仓库包含可执行实现、离线中英文模型、自动化测试和打包工具。Windows 与 Wayland 屏幕访问仍需要交互式实机验收，具体执行记录见 [TESTING.md](TESTING.md)，缺项见 [LIMITATIONS.md](LIMITATIONS.md)。

## 已实现

- Windows GDI / Linux X11 XCB+RandR / Wayland ScreenCast Portal + PipeWire 截图后端；在显示选区前采集桌面，保留可捕获的普通菜单。
- 主屏幕、指定显示器、整个桌面；矩形、窗口边界选择、椭圆、多边形、自由曲线；延时及倒计时取消。Wayland 可授权多个显示器，在各屏冻结画面上框选窗口内容或区域，也支持全桌面拼接。
- 原生或 Portal 全局快捷键、冲突提示、单实例命令转发、托盘和独立启动器入口。
- 矩形、椭圆、直线、箭头、画笔、荧光笔、文字、序号、马赛克、近似模糊、裁剪、旋转、翻转；标注颜色、线宽、字体、透明度、移动、修改和删除；撤销/重做。
- `.snapling` 版本化工程保存原图、编辑指令、OCR 和翻译布局；PNG 透明导出、JPG 背景色；异步保存图片。
- 按需初始化的 Tesseract 5 LSTM 本地 OCR，中文/英文/混合；直接在图像上拖选、Unicode 复制、全选、逐行选择、TXT/Markdown 导出。**目前没有接入 RapidOCR/ONNX Runtime**。
- Ollama / OpenAI 兼容 HTTP API：供应商配置、环境变量密钥、连接测试、异步翻译、取消、超时、ID 校验、原地译文渲染和图片导出。
- 简单背景估色覆盖、复杂背景底板、字号自适应、手动调整译文；容纳不了的文字阻止图片导出，工程仍保留完整内容。
- 贴图、HEX 取色、ZXing 二维码识别、需确认的敏感信息打码建议、可关闭历史、主题与用户登录自动启动。
- Wayland 普通截图按需开启 ScreenCast / PipeWire，取得完整屏幕首帧后结束临时共享，再显示 SnapLing 选区。用户也可主动开启持续共享以复用单帧缓存；支持状态提示、停止共享及 restore token，尚未完成授权后的实机验收。

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
snapling --window         # 窗口边界选区；Wayland 在冻结屏幕上框选窗口内容
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
2. 拖动框选，松开鼠标即显示选区周围的深色图标工具栏。工具使用原创矢量图形，悬停显示名称；颜色、线宽、字体和透明度在样式图标中调整。窄屏自动排成两行。多边形逐点单击，双击闭合后开始即时编辑。
3. 在当前选区直接使用矩形、椭圆、箭头、直线、画笔、文字、序号、马赛克和模糊。「选择」工具移动对象，Delete 删除，颜色、线宽与字体可修改对象样式。Ctrl+Z / Ctrl+Shift+Z 撤销与重做。
4. 文字直接在选区内输入，Enter 换行，Ctrl+Enter 或点击输入框外完成输入，Esc 取消本次文字输入。
5. 点击 OCR 后直接拖选图中文字，Ctrl+C 优先复制选中的文字，Ctrl+A 全选。点击翻译图标后按钮变绿，译文叠加到当前文字位置；再次点击取消未完成任务并恢复原图，标注和已有译文图层保留。再次开启可显示已完成的译文。对照图标菜单提供原文、译文和对照，可复制译文或图片、保存 PNG/JPG。
6. 按 Enter 或点击 ✅ 打开独立高级编辑器，同一份文档保留全部标注、OCR、翻译、撤销与重做历史，可继续裁剪、旋转、翻转、精调译文、保存工程或贴图。复制和保存会保留即时编辑界面，❌ 或 Esc 取消截图。

Wayland 先取得所选显示器的完整画面，再显示各屏的 SnapLing 全屏选区。松开鼠标时，工具栏直接出现在当前屏幕的同一遮罩内。首次屏幕访问仍需系统授权，后续是否再次询问由桌面决定。普通截图在取得各屏首帧后结束临时共享；主动开启的持续共享保留到用户停止。未启用 PipeWire 的构建仅尝试单屏非交互式 Screenshot，多屏显示依赖提示。

即时编辑与高级编辑共享 `Document`、`Canvas` 和 `EditorSession`。OCR、翻译和图片保存异步运行，可在任务执行中继续标注、撤销、复制或进入高级编辑器。错误显示在当前界面的状态提示中。远程翻译通过非阻塞确认控件显示发送目的地。

## 多显示器（Ubuntu Wayland）

1. 主界面的屏幕选项默认是「整个桌面（全部屏幕）」。只截某块外接屏时，可在下拉框选择对应的 HDMI/DP 显示器，列表同时显示尺寸和缩放比例。
2. 点击「重新选择共享屏幕…」，在 Ubuntu 系统授权窗口勾选需要的内置和外接屏幕。选择整个桌面时需勾选全部显示器；升级后的首次截图会重新选择，避免沿用旧的单屏授权。
3. 按 Ctrl+Alt+A，各屏分别显示冻结选区；在需要的屏幕拖动，松开立即标注，其他屏幕遮罩关闭。Esc 取消整个截图。
4. 使用「全屏」模式和「整个桌面」可得到按屏幕布局拼接的图片，在一个选区中按比例预览。单屏区域保留该屏原生像素，全屏拼接按最高像素密度统一输出。

若提示某块显示器未授权，请重新选择并勾选该屏。程序会说明缺失的屏幕。区域框选目前在一块屏幕内完成，跨屏连续拖动框选尚未实现。三台物理屏幕的 GNOME 授权与覆盖层仍需实机验证，自动化范围见 [TESTING.md](TESTING.md)。

## 离线 OCR

模型随源码提供，约 6.4 MiB，安装至 `share/snapling/tessdata`，无需联网下载。首次点击 OCR 才加载模型。手动导入时选择含 `eng.traineddata` / `chi_sim.traineddata` 的目录。来源、许可证、大小和 SHA256 见 [models/README.md](models/README.md)。

识别后直接拖动图中文字；双击选择连续字词，Ctrl+双击选择整行。「选全部文字」「复制全部文字」「导出文字」可保留换行。行框来自引擎，字符位置依据字宽估算，不代表精准字符检测。OCR 高亮不会写入导出的图片。

截图 OCR 使用分散文本模式，预处理深色画面和小字号，并将识别坐标还原到原图像素。相距较远的文字分成独立文字框，避免把弹窗菜单与背景内容拼成一行。

## AI 翻译配置

默认本机 Ollama：Base URL `http://127.0.0.1:11434/v1`，模型 `qwen3.5:4b`。模型须由用户在 Ollama 中预先安装；SnapLing 不启动、下载或常驻加载大模型。

设置 → AI 翻译：配置地址、模型、目标语言、超时、temperature、token 上限，可添加/更新/删除供应商。支持兼容 `/chat/completions` 的服务，连接测试使用 `/models`。不支持 JSON 模式的服务可关闭 `response_format`，但仍需返回可解析的块 ID JSON。

本机服务默认开启「关闭思考模式」，通过 `reasoning_effort: none` 避免 Qwen 等模型在生成译文前耗尽输出 token。兼容服务不支持此参数时可取消勾选。生成被截断、仅返回思考过程或返回空内容时，应用会显示对应原因。

密钥使用环境变量 `SNAPLING_API_KEY`（可改变量名），或本次运行有效的临时密钥。密钥不写入配置和日志。远程服务发送前明确显示目标地址并确认；本机服务也明确显示地址。只有主动翻译会发送 OCR 文字，不发送截图图片，不提供工具执行能力。

自动翻译保留路径、项目列表中路径上方的名称、技术日志键值、目标语言文字和低置信度内容；需要单独处理时可先在 OCR 模式选择文字。译文沿原文字框排列，按原文字高度估计字号，最多自动缩小约 15%。放不下时保留原文并提示在高级编辑器调整，不会自动扩宽覆盖相邻内容。原字号估计和语言判断存在限制，详见 [LIMITATIONS.md](LIMITATIONS.md)。

高级编辑器可调整译文、字号、区域高度和背景底板。保存 PNG/JPG 与复制图片包含当前启用的译文，关闭翻译后输出原图及标注。「保存工程」保留可再次修改的数据。

## 隐私与部署

历史默认关闭，仅启用后在图片导出时写入历史。OCR 和翻译不会自动运行；搜索、网络翻译均须主动触发。Wayland 截图触发临时屏幕访问，持续共享由用户单独开启。工程保留原始图像，**打码后的工程仍含未打码原图**，对外分享请导出 PNG/JPG。

包构建：`tools/package-linux.sh`；AppImage：`packaging/linux/appimage.sh`；Windows 部署与安装脚本位于 `packaging/windows`。不同 Ubuntu 版本应在对应发行版或更老基线上构建，不能将 Ubuntu 26.04 编译的程序直接宣称兼容 24.04。

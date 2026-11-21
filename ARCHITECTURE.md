# 架构

`src/app` 管理生命周期、命令行、单实例、主界面与截图调度。`core/Types` 保存任务取消标记、屏幕帧、OCR 行和翻译单元。GUI 不直接调用 Win32、Xlib、XCB 或 PipeWire。

## 截图与平台

`ICaptureBackend` 提供异步 `capture`、`cancel` 和结果/错误信号；工厂根据实际 Qt 平台及 Wayland 会话选择实现。

- Windows：工作线程使用 GetDC / BitBlt / GetDIBits，原生资源由 RAII 释放。每个显示器查询物理矩形，再拼接到统一像素密度的逻辑桌面。窗口矩形来自 EnumWindows 与 DWM 扩展边界。当前窗口捕获是可见桌面裁切，没有 WGC 独立离屏窗口捕获。
- X11：RandR 匹配显示器名称与物理坐标；独立 XCB 连接获取根窗口像素与窗口边界。窗口查询失败不会触发 Xlib 默认致命错误。高 DPI 和负逻辑原点映射由 CaptureFrame 统一处理。
- Wayland 默认请求 Screenshot(interactive=false, modal=false)。工作线程检查本地 URI 图像尺寸、解码完整桌面，再以 QScreen 布局联合矩形和统一像素比例拆分各屏；不打开 ScreenCast 会话。长宽比或当前布局不匹配时报告错误，不自动打开系统框选或共享。未编译 PipeWire 时同样支持多屏。
- 单次截图由 PortalConnection 持有独立 D-Bus 连接，第一条发往 Desktop Portal 的调用是 Host Registry.Register(io.snapling.SnapLing)。注册成功后才在同一连接发出 Screenshot。PortalRequest 可注入连接，Request 路径、Response 订阅和 Close 均使用该连接，避免混用 Qt 默认会话连接的身份。成功注册可复用；服务重启后重新注册，注册错误后的重试更换干净连接。旧 Portal 不支持 Registry 时明确标记 legacy_unverified，依靠系统启动身份；普通注册错误不会回退到匿名请求。取消与销毁通过 generation 和 QObject 上下文隔离迟到回调。热键和兼容 ScreenCast 继续使用原连接。
- 设置中的 ScreenCast 兼容模式执行 CreateSession → SelectSources（multiple=true）→ Start，按元数据解析和过滤节点，为每一路独立调用 OpenPipeWireRemote。异步等待所需各屏首帧，在工作线程拼接后关闭临时共享。首帧等待上限 10 秒；每次仅复制第一帧，然后暂停流。
- PortalRequest 在方法调用前订阅 Response，支持 Close、超时和旧路径兼容。运行时接口版本异步查询（2.5 秒），方法调用超时 5 秒，用户授权响应超时 120 秒；快捷键 CreateSession 单独限制 10 秒。PipeWire 流以 INACTIVE 创建，仅截图时激活，图像由 mutex 保护。保留的兼容后端会话也在空闲时暂停，下一次截图清除旧缓存重新取帧；正常 UI 不提供持续共享入口。
- ScreenCast 的 position/size 与 Qt 屏幕布局匹配，必要时使用与 QScreen 名称相同的来源标识；只有单屏允许缺失元数据回退。多屏无法匹配或授权不全时明确报错。旧单屏 restore token 不复用，多屏 token 单独存储。兼容模式的「重置兼容截图授权」清除选择并开始新截图。显示器变化关闭旧流并清除选择；请求由 generation 隔离，Closed 检查当前会话路径。

`CaptureFrame.screens` 保留各屏名称、全局逻辑矩形和原生像素。`combineCaptureScreens` 按最高像素密度拼接完整桌面，处理负原点与屏幕间空隙，限制输出至 1 亿像素。区域截图为每个输出创建独立 `SelectionOverlay`，直接使用各屏原生图像；在一屏完成选区后关闭其他遮罩，再继续该屏的同一编辑会话。全屏拼接图在一个输出上按比例预览。物理多屏与混合 DPI 仍需实机验证。

后台快捷键触发直接提交采集任务，采集结束才创建选区。由本程序按钮触发时先隐藏本程序窗口、等待合成器更新；延时结束也先关闭倒计时。最终图像从捕获帧生成，不对选区窗口自身截屏。首次系统授权不在本程序控制范围内。

`IHotkeyBackend` 隔离 RegisterHotKey、XGrabKey 和 GlobalShortcuts Portal。Portal 会话按 QDBusObjectPath 解码，preferred_trigger 使用 XDG keysym，绑定返回值验证后才标记注册成功；重绑取消旧请求，Activated/Closed 按会话隔离。activation_token 经信号或命令 IPC 传递给 QtWayland 激活请求，不写日志或配置。Wayland 覆盖层使用普通全屏 Window，每个输出独立 setScreen。QLockFile 与用户专属 QLocalServer 防止多实例竞态；IPC 有 64 KiB 上限、长度边界和连接超时。主界面提供系统快捷键命令及重试，`--cancel-capture` 可恢复未完成的截图。

## 编辑与图像

`SelectionOverlay` 管理 Selecting、QuickEditing、AdvancedEditing 和 Cancelled 阶段。鼠标进入当前屏幕预选整屏，离开清除预选。单击选择整屏；超过系统拖动阈值后以按下/释放坐标框选，即使缺少 MouseMove 事件也有效。预选与移动标注分离。鼠标松开在同一遮罩中立即创建 Canvas 和浮动工具栏。全屏模式直接开始 QuickEditing。Enter 或 ✅ 将同一个 EditorSession 交给 EditorWindow。

`Document` 持有选区原图、标注列表、裁剪、旋转、翻转、OCR、翻译图层及历史。两个阶段持有同一个文档实例，撤销/重做保存隐式共享的状态，最多 100 步。`AnnotationTools` 在两个阶段复用工具和样式控制。

`ToolIcons` 以原创 24×24 矢量路径生成多尺寸 QIcon，包含普通、选中、禁用状态。即时工具栏使用纯图标、工具提示和可访问名称，样式属性收进弹出面板；窗口宽度不足时分为两行，布局计算包含边框和控件最小宽度，保留确认入口。

`EditorSession` 持有文档与异步 OCR、翻译、图片保存任务。计算状态 Idle、Ocr、Translating 独立于界面阶段和当前工具。高级窗口接管同一个会话后，未完成任务继续更新同一文档。界面销毁时 QObject 连接自动断开，最终会话释放会取消识别与网络请求。结果应用前检查源图身份，远程发送使用已确认的供应商配置快照。

翻译启用状态由会话共享，驱动两个界面的绿色按钮。关闭时取消 OCR/网络请求、清除后续翻译意图并切回原图，已完成图层保留；再次开启可复用同目标语言的图层。关闭状态不接收迟到译文，撤销/重做与视图模式会同步按钮状态。

`Canvas` 负责坐标逆变换、标注命中、拖动预览、内嵌文字输入和 OCR 文字选择。覆盖层按选区映射原图像素，独立窗口使用缩放视图。文字输入处理 Enter 和编辑快捷键，退出输入后 Enter 才确认截图。高级窗口打开或取消截图时恢复先前窗口，最多保留 8 个高级编辑器。标注在导出/重建时绘制，原图保持独立。

`.snapling` v1 是 JSON 与 Base64 PNG 的自包含工程（目前不是 ZIP）。QSaveFile 原子写入；加载时检查格式版本、文件大小、解码尺寸、坐标与标注类型。未来版本拒绝打开，读取失败不破坏当前文档。磁盘占用上限 384 MiB、图像几何上限 1 亿像素；Qt 解码器还会应用自身内存限制。

## OCR

`IOcrEngine` 支持默认 Tesseract 5 LSTM 和可选 RapidOCR、PaddleOCR PP-OCRv6 Small。Tesseract 在 QtConcurrent 工作线程按需初始化，使用 mutex 保护可缓存的引擎状态。输入在工作线程合成透明背景并灰度化，对以暗色为主的画面反色，在原图不超过 4 Mi 像素时放大两倍并添加留白。PSM_SPARSE_TEXT 识别多窗口分散文字，按词间距分开远隔文字并汇总置信度。边距与缩放统一逆映射回原图像素。ETEXT_DESC 提供取消、45 秒期限和实际进度，GUI 用定时器读取原子进度。

RapidOCR 和 PaddleOCR 共用 QtConcurrent 的图片准备和 QProcess 的本地推理流程，识别结束后清理私有临时图片。PaddleOCR 加载用户模型目录的 ONNX，再用对应的 YAML 配置检测阈值、方向分类预处理和 CTC 字典。识别文字、置信度和四点坐标通过 JSON 返回，与 Tesseract 一样映射为 `OcrLine`，供选字和翻译使用。

每个 OcrLine 包含文本、行矩形、多边形、置信度、阅读顺序、段落、方向字段和字符估算标记。当前方向为水平，垂直/复杂版面未专项支持。TextLayout 使用 Unicode grapheme 划分，不拆开 surrogate pair 或组合字符；字符宽度来自字体度量，框明确标记为估算值。鼠标拖选得到结构化的行区间，翻译选中文字时只发送对应区间。

## 翻译

`ITranslationProvider` 的实现通过 QNetworkAccessManager 异步访问 Chat Completions。Ollama 使用兼容 API；供应商地址与模型均可配置。Provider 没有命令、文件、浏览器或设置修改工具。截图文字作为不可信 JSON 数据发送，system 提示和原文分离。

返回解析允许 JSON 对象、数组或 Markdown 围栏；所有块 ID 必须完整、唯一、无未知项，全部验证后才更新结果。HTTP 状态、请求取消、超时、响应大小均检查。错误消息不包含响应正文、OCR 文本或密钥。

自动请求过滤置信度低于 0.55 的内容、路径/文件名、项目路径对应标题、代码标识、日志键值和可判定为目标语言的文字；显式选择的 OCR 文本可单独提交。请求的字号由原文字框高度估计，模型仅返回 ID 和文本，坐标和字号由客户端保留。

译文作为独立 TranslationUnit 保存；BasicImageProcessor 根据行框周围颜色估算背景，复杂背景和用户底板按深浅色选择，文字对比色依据实际填充色计算。QTextLayout 计算换行，字形边界与绘制基线共用，整个绘制裁剪在原文字框内。新译文最多自动缩小约 15%，无法容纳时保留原文、标红提示并阻止不完整图片导出。与原文相同的结果保留原像素。复杂纹理修复接口可替换，目前不包含 OpenCV Inpainting。对照模式通过原图侧栏实现，导出输出当前译文视图。

## 线程、内存和隐私

- GUI、剪贴板、窗口、Portal 和 HTTP 事件在 GUI 线程。
- 原生采集和截图解码使用专用单线程池复用缓冲分配环境；OCR、图片渲染编码及保存在线程池。
- PipeWire 自有线程，图像缓存由 mutex 保护；不排队发送帧，不保存视频。
- OCR worker 捕获共享状态及取消 token，不捕获 GUI 裸指针。窗口销毁取消任务，晚到的 QObject 回调自动断开。
- 翻译网络事件无同步等待；显式超时和取消。图片保存使用冻结状态，保存期间允许继续编辑。
- API Key 只在进程内或环境变量，供应商档案不保存密钥。日志仅记录耗时、数量和错误代码。
- 历史默认关闭，开启后才持久化导出的图片；没有后台 OCR、后台 AI 或截图上传。
- Linux 默认 generic Qt 主题避免 GTK 间接加载 LLVM；用户可通过 QT_QPA_PLATFORMTHEME 覆盖。

配置文件采用 schemaVersion=1，缺省键自动补默认值；升级入口集中于 Settings 构造函数。平台登录自动启动操作封装在 Platform/Autostart。滚动截图、表格提取尚未实现，扩展应通过新服务组合现有捕获与 OCR 数据，不往 Canvas 中加入原生输入模拟。

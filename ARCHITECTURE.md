# 架构

`src/app` 管理生命周期、命令行、单实例、主界面与截图调度。`core/Types` 保存任务取消标记、屏幕帧、OCR 行和翻译单元。GUI 不直接调用 Win32、Xlib、XCB 或 PipeWire。

## 截图与平台

`ICaptureBackend` 提供异步 `capture`、`cancel` 和结果/错误信号；工厂根据实际 Qt 平台及 Wayland 会话选择实现。

- Windows：工作线程使用 GetDC / BitBlt / GetDIBits，原生资源由 RAII 释放。每个显示器查询物理矩形，再拼接到统一像素密度的逻辑桌面。窗口矩形来自 EnumWindows 与 DWM 扩展边界。当前窗口捕获是可见桌面裁切，没有 WGC 独立离屏窗口捕获。
- X11：RandR 匹配显示器名称与物理坐标；独立 XCB 连接获取根窗口像素与窗口边界。窗口查询失败不会触发 Xlib 默认致命错误。高 DPI 和负逻辑原点映射由 CaptureFrame 统一处理。
- Wayland：只通过 XDG Portal 访问桌面。PortalRequest 在方法调用前订阅 Response，避免响应竞态；支持请求 Close、超时和旧路径兼容。普通截图被系统拒绝时重试交互式选择器；取消不重试。系统返回的本地 URI 在工作线程解码。
- 快速路径：用户明确开启后，CreateSession → SelectSources → Start → OpenPipeWireRemote。授权恢复 token 每次更新；pw_thread_loop 负责视频回调，最多保留一帧。状态可见，停止时销毁流、远程 core/context 和会话。只接受可映射的 CPU RGB 帧；DMA-BUF 等不可用格式报错并可降级 Portal。

后台快捷键触发直接提交采集任务，采集结束才创建选区。由本程序按钮触发时先隐藏本程序窗口、等待合成器更新；延时结束也先关闭倒计时。最终图像从捕获帧生成，不对选区窗口自身截屏。首次系统授权不在本程序控制范围内。

`IHotkeyBackend` 隔离 RegisterHotKey、XGrabKey 和 GlobalShortcuts Portal。QLockFile 与用户专属 QLocalServer 防止多实例竞态；IPC 消息有 64 KiB 上限、长度边界和连接超时。GNOME 自定义快捷键可调用同一命令入口。

## 编辑与图像

`SelectionOverlay` 在冻结帧上选择路径，导出时按路径生成透明 Mask。`Document` 独立持有原图，以及标注列表、裁剪、旋转、翻转、OCR 和翻译。撤销/重做保存隐式共享的状态，最多 100 步；原图不会随每条指令复制。

`Canvas` 负责坐标逆变换、标注命中、拖动预览和文字高亮。只保留一张当前渲染缓存。截图后恢复先前编辑窗口，最多同时保留 8 个截图编辑器，避免隐藏窗口持续积累原图。标注在导出/重建时绘制，不写入原图；马赛克、近似模糊只计算局部图像。

`.snapling` v1 是 JSON 与 Base64 PNG 的自包含工程（目前不是 ZIP）。QSaveFile 原子写入；加载时检查格式版本、文件大小、解码尺寸、坐标与标注类型。未来版本拒绝打开，读取失败不破坏当前文档。磁盘占用上限 384 MiB、图像几何上限 1 亿像素；Qt 解码器还会应用自身内存限制。

## OCR

`IOcrEngine` 的当前生产实现为 Tesseract 5 LSTM，未接入 RapidOCR。模型在 QtConcurrent 工作线程按需初始化；缓存受设置控制，使用 mutex 保护引擎状态。输入 QImage 隐式共享，仅在推理前转换 RGB。ETEXT_DESC 提供取消、45 秒期限和实际进度，GUI 用定时器读取原子进度。

每个 OcrLine 包含文本、行矩形、多边形、置信度、阅读顺序、段落、方向字段和字符估算标记。当前方向为水平，垂直/复杂版面未专项支持。TextLayout 使用 Unicode grapheme 划分，不拆开 surrogate pair 或组合字符；字符宽度来自字体度量，框明确标记为估算值。鼠标拖选得到结构化的行区间，翻译选中文字时只发送对应区间。

## 翻译

`ITranslationProvider` 的实现通过 QNetworkAccessManager 异步访问 Chat Completions。Ollama 使用兼容 API；供应商地址与模型均可配置。Provider 没有命令、文件、浏览器或设置修改工具。截图文字作为不可信 JSON 数据发送，system 提示和原文分离。

返回解析允许 JSON 对象、数组或 Markdown 围栏；所有块 ID 必须完整、唯一、无未知项，全部验证后才更新结果。HTTP 状态、请求取消、超时、响应大小均检查。错误消息不包含响应正文、OCR 文本或密钥。

译文作为独立 TranslationUnit 保存；BasicImageProcessor 根据行框周围颜色估算背景，复杂背景采用近纯色底板。排版先缩字、再扩展区域；无法完整容纳时显示红框并阻止图片导出。复杂纹理修复接口可替换，目前不包含 OpenCV Inpainting。对照模式通过原图侧栏实现，导出输出译文图。

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

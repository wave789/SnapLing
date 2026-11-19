# 实现时核对的官方资料

- [Screenshot Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Screenshot.html)：interactive、版本化 target、异步 URI 结果。
- [Request 协议](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Request.html)：handle_token、预先订阅 Response、Close。
- [ScreenCast Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)：会话、persist_mode、单次 restore token、PipeWire FD。
- [GlobalShortcuts](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)：创建会话、绑定 action、Activated。
- [PipeWire 视频采集](https://docs.pipewire.org/page_tutorial5.html)：格式协商、dequeue/queue 缓冲区与流生命周期。
- [Tesseract C++ API](https://tesseract-ocr.github.io/tessdoc/APIExample.html)：识别、ResultIterator、边界与置信度。
- [RapidOCR 上游](https://github.com/RapidAI/RapidOCR)：已评估，目前使用 Tesseract 作为独立生产后端，未声称集成 RapidOCR。
- [Win32 GetDIBits](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdibits)：读取前将 bitmap 从 DC 中解除选中。
- [Qt QScreen](https://doc.qt.io/qt-6/qscreen.html)：逻辑几何与像素比例的区别。
- [GNOME Portal 实现](https://gitlab.gnome.org/GNOME/xdg-desktop-portal-gnome)：交互式截图由 Shell 提供，不能绕过授权。

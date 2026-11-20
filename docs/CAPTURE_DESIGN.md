# 单次冻结桌面与多屏交互

## 微信现象能够说明什么

在三块屏幕播放视频时触发截图，画面停留、声音继续，说明截图界面展示了某个时刻的静态图像，覆盖在仍运行的应用上。鼠标移入哪块屏幕就显示那块屏幕的边框，说明“获取桌面图像”与“根据鼠标位置选择屏幕或区域”是两个步骤。显示器有几个，只会影响图像范围和坐标映射，不必让用户分别启动截图。

微信未公开这部分实现。仅凭可见行为不能断言 Linux 微信用了哪个私有接口、是否有系统集成或与 Windows 相同的实现。SnapLing 复现其交互，使用系统允许的接口实现取图，不冒称已确认微信源码。

## Ubuntu GNOME 可用的实现路径

默认请求 `org.freedesktop.portal.Screenshot.Screenshot`，选项为 `interactive=false`、`modal=false`。取得完整桌面的本地 PNG 后，在工作线程解码，再按 Qt 屏幕的全局逻辑位置拆成各屏画面。画面只有一份捕获时间，不需要多个屏幕分别授权 ScreenCast。各屏创建普通全屏 Wayland surface，设置对应 QScreen；实际播放器继续运行。

鼠标进入屏幕时预选该屏全图并显示绿色边框。鼠标左键按下后，根据按下和释放位置区分单击与拖动：单击选整屏，拖动选区域。释放时在原 surface 内显示工具栏及 Canvas，不再次采集屏幕，不要求 Enter。Enter/✅ 才转入独立高级编辑器，并保留同一个文档。多个覆盖层只请求一次键盘激活，其他窗口显示时不争抢焦点；事件以窗口内坐标映射到对应冻结图像，不依赖 Wayland 中不保证准确的全局窗口位置。

该路径不创建 ScreenCast/PipeWire 流，空闲时没有视频帧复制，编辑的是冻结图像。三屏意味着一次截图需要处理更多像素和更多静态图像内存，并不意味着三路视频一直复制。Portal 路径有 PNG 编码、文件读取和解码成本，截图文件由桌面后端管理。兼容模式只临时使用 ScreenCast，取得首帧后关闭会话；内部保留会话的代码也会在无截图请求时暂停流，每次请求只复制第一帧。

## 首次权限和接口边界

“不共享屏幕”不等于“绕过系统截图权限”。首次使用可能出现截图授权，授权后的行为由当前桌面实现决定。上游 xdg-desktop-portal 的非交互式路径查询 `screenshot` 权限存储；已允许时不再请求 AccessDialog，并向 GNOME 后端传入 `permission_store_checked`。GNOME 的该路径直接取得整桌面图像，跳过截图预览对话框。

SnapLing 不请求 `interactive=true`，避免先在 Ubuntu 工具中框选、再进入 SnapLing 框选。拒绝权限时保留错误，不自动启动共享或系统框选。不同桌面/版本可能不提供完整桌面图像，因此设置中保留用户主动选择的兼容模式。

截图发生在系统处理请求的时刻，有 API、编码和解码延迟；首次授权期间不能保证捕获快捷键按下那一帧。保护视频也可能返回黑色画面。当前不支持跨多个输出连续拖动一个选区。

上游依据（实现会随版本变化）：

- [Screenshot Portal 接口](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Screenshot.html)
- [xdg-desktop-portal 1.20.3 的权限及后端转发](https://github.com/flatpak/xdg-desktop-portal/blob/1.20.3/src/screenshot.c)
- [GNOME 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/main/src/screenshotdialog.c)：`skip_dialog`、`maybe_show_screenshot`、`do_take_screenshot`
- [GlobalShortcuts Portal 接口](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)：会话、preferred_trigger 和 activation_token
- [QtWayland 激活处理](https://github.com/qt/qtwayland/blob/6.8/src/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp)：`XDG_ACTIVATION_TOKEN`

## 注册超时与排查

`qt.qpa.services: Failed to register with host portal ... NoReply` 来自 Qt 向 host Portal 注册应用身份，表示系统服务未及时回复；它与 Screenshot 请求、GlobalShortcuts 注册是不同操作。不能只凭该行认定快捷键或截图成功。源码运行也应保留正确的 `io.snapling.SnapLing` 桌面身份，正式安装包会安装对应 desktop 文件。

SnapLing 不再在主界面初始化时同步查询三个 Portal 版本；快捷键状态独立显示，注册失败可重试或复制绝对路径命令，在 Ubuntu 自定义快捷键中绑定。Portal Activated 以及命令启动的 activation token 传给 QtWayland，用于请求激活截图层。

卡住的截图可从托盘取消，或执行 `./tools/run.sh --cancel-capture`。如继续出现系统服务超时，可在用户终端查看：

```bash
systemctl --user status xdg-desktop-portal.service xdg-desktop-portal-gnome.service --no-pager
journalctl --user -u xdg-desktop-portal.service -u xdg-desktop-portal-gnome.service -n 80 --no-pager
```

程序不自动重启桌面服务或修改系统权限。自动化范围和未验证硬件见 [TESTING.md](../TESTING.md)。

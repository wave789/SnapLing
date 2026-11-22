# 保留 GNOME 动画并缩短截图等待

## GNOME 快速框选

安装新版 `.deb` 后，若已有旧扩展在运行，注销并重新登录一次。SnapLing 启动时会检查并尝试启用随包的 GNOME 扩展；默认截图方式已迁移为 **GNOME 即时框选**。可在 **设置 → 通用 → GNOME 即时框选** 查看扩展状态，不需要手动配置模型或截图权限。

在主界面选「整个桌面（全部屏幕）」和「矩形区域」。按快捷键后，GNOME Shell 冻结桌面并显示绿色框。单击选择指针所在的整块屏幕，或按住左键拖动选区。松开后 SnapLing 立即显示完整即时编辑工具栏，同时保留整块屏幕冻结图和八个手柄；可反复拖动框内移动选区，或拖动边缘、角点调整大小，工具栏跟随选区摆放。选择工具、按 Enter 或点击 ✅ 后显示 Canvas。即时编辑中的 Enter 和 ✅ 进入独立高级编辑器。

选区出现前，GNOME Shell 生成供合成器显示的桌面快照。鼠标进入已冻结的屏幕时，提前在后台准备该屏图像，松手时将它与最终选区坐标一并交给 Qt。冻结画面保留到带工具栏的编辑器首帧显示，随后所选屏幕继续显示冻结画面及遮罩，其他屏幕恢复实时显示。该模式依赖已启用的 GNOME 48–50 扩展。当前用户拥有的开发构建 `build/snapling`、用户安装和正式安装均可请求即时框选；指定单屏和其他截图形状按各自路径选择。两种截图方式都无需持续屏幕共享。

第 17 版使用 GNOME 的原生异步 PNG 导出，在框选期间准备整屏图像，保留松手后移动和放大选区的能力。Qt 在映射窗口前准备工具栏。冻结后续屏幕时只临时移除截图内容的绘制，输入遮罩始终保持映射，避免反复切换全屏输入层。冻结前会隐藏请求进程拥有的窗口 actor，避免主窗口淡出画面进入截图。

## 系统单次截图提速

GNOME 的单次截图 Portal 会等截图文件和闪光动画都完成，再把图片交给 SnapLing。此前的透明 CSS 只隐藏白色背景，约 500 毫秒的闪光淡出仍在运行。两个任务并行，因此缩短的时间取决于截图编码与动画的重叠程度。[GNOME 50 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/screenshotdialog.c#L247-L255) · [GNOME Shell 50.1 的截图和过渡](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/screenshot.js#L2668-L2684)

可选扩展跳过 GNOME Shell 单次截图的闪光过渡。第 3 版也跳过 SnapLing 全屏截图覆盖层的窗口动画，其他窗口和桌面仍保留动画。扩展使用 GNOME Shell 的内部方法，版本声明限定在 GNOME 48 至 50。截图授权、取图和快门声仍由系统处理。[GNOME Shell 50.1 窗口入场流程](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/windowManager.js#L1436-L1488)

SnapLing 的 `.deb` 已附带该扩展，应用启动时会自动尝试启用。首次安装后若 GNOME 尚未发现扩展，注销并重新登录一次。升级扩展后也需重新登录，让 GNOME 加载新版代码。扩展不可用时 SnapLing 会自动回退到系统单次截图，因此截图入口仍然可用，但不会达到即时响应的速度。

如果之前按旧版说明安装过透明样式，GNOME 会优先使用用户目录里的同名扩展。在源码目录运行下面的命令可以更新旧版。源码运行者也可以用这条命令为自己安装扩展。

```bash
extension_id='snapling-no-flash@snapling.local'
extension_dir="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$extension_id"
install -d -- "$extension_dir"
install -m 644 -- tools/gnome-fast-screenshot/{metadata.json,extension.js,desktopSnapshot.js,regionSelector.js,screenshotBridge.js,stylesheet.css} "$extension_dir/"
```

已经用 `tools/install.sh` 安装的用户，可将上面命令里的 `tools/gnome-fast-screenshot` 替换成 `~/.local/snapling/share/gnome-shell/extensions/snapling-no-flash@snapling.local`。

注销并重新登录一次，让 GNOME 加载新版扩展。启用后，可以把之前关闭的界面动画恢复为默认值。

```bash
gnome-extensions enable snapling-no-flash@snapling.local
gsettings reset org.gnome.desktop.interface enable-animations
```

可在 SnapLing 设置里停用提速，也可以运行 `gnome-extensions disable snapling-no-flash@snapling.local` 恢复原来的过渡。

截图仍需要冻结、绘制和最终编码所选区域。`trigger_to_quick_edit_ms` 记录 GNOME 框选完成后到 Qt 首次绘制即时编辑层。系统回退路径的 `portal_round_trip_ms`、`image_prepare_ms` 和 `trigger_to_selection_ms` 可帮助定位慢点。GNOME 的首帧显示还受合成器呈现时机影响。

在嵌套 GNOME 48.7 和 Ubuntu 26.04 Portal 前端的三台 800×600 虚拟屏上，每种配置重复三次，保留全局动画。旧透明样式下，首个选区在命令触发后约 701 毫秒可见。启用提速扩展后，中位数约为 255 毫秒。测量包括截图命令转发和合成器显示时间。已核对 GNOME 49 和 50.1 的内部接口，物理 Ubuntu 26.04 多屏速度随分辨率、画面及硬件变化。

在三台 1920×1080 虚拟屏的高细节测试画面中，即使跳过闪光，GNOME 交付整张 PNG 仍用了约 0.7–0.9 秒。要让框选出现在 PNG 编码之前，需要在合成器里先持有冻结画面并显示选区。GNOME 自带的 `Print` 工具采用这种顺序，用户确认后才导出图片。非交互式 Screenshot Portal 在交出图片前没有可供 SnapLing 绘制的截图帧。

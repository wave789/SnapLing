# 保留 GNOME 动画并缩短截图等待

GNOME 的单次截图 Portal 会等截图文件和闪光动画都完成，再把图片交给 SnapLing。此前的透明 CSS 只隐藏白色背景，约 500 毫秒的闪光淡出仍在运行。两个任务并行，因此缩短的时间取决于截图编码与动画的重叠程度。[GNOME 50 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/screenshotdialog.c#L247-L255) · [GNOME Shell 50.1 的截图和过渡](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/screenshot.js#L2668-L2684)

可选扩展跳过 GNOME Shell 单次截图的闪光过渡。第 3 版也跳过 SnapLing 全屏截图覆盖层的窗口动画，其他窗口和桌面仍保留动画。扩展使用 GNOME Shell 的内部方法，版本声明限定在 GNOME 48 至 50。截图授权、取图和快门声仍由系统处理。[GNOME Shell 50.1 窗口入场流程](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/windowManager.js#L1436-L1488)

SnapLing 的 `.deb` 已附带提速扩展。安装后可以直接使用默认截图。需要提速时，打开 **设置 → 通用 → 单次截图提速**，点击 **启用**。首次安装后若 GNOME 尚未发现扩展，注销并重新登录一次。升级后也需重新登录，让 GNOME 加载新版代码。已启用的扩展会保留启用状态。

如果之前按旧版说明安装过透明样式，GNOME 会优先使用用户目录里的同名扩展。在源码目录运行下面的命令可以更新旧版。源码运行者也可以用这条命令为自己安装扩展。

```bash
extension_id='snapling-no-flash@snapling.local'
extension_dir="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$extension_id"
install -d -- "$extension_dir"
install -m 644 -- tools/gnome-fast-screenshot/{metadata.json,extension.js,stylesheet.css} "$extension_dir/"
```

已经用 `tools/install.sh` 安装的用户，可将上面命令里的 `tools/gnome-fast-screenshot` 替换成 `~/.local/snapling/share/gnome-shell/extensions/snapling-no-flash@snapling.local`。

注销并重新登录一次，让 GNOME 加载新版扩展。启用后，可以把之前关闭的界面动画恢复为默认值。

```bash
gnome-extensions enable snapling-no-flash@snapling.local
gsettings reset org.gnome.desktop.interface enable-animations
```

可在 SnapLing 设置里停用提速，也可以运行 `gnome-extensions disable snapling-no-flash@snapling.local` 恢复原来的过渡。

截图仍需要取图、编码、解码和绘制。`portal_round_trip_ms` 记录从发送截图请求到 Portal 返回。`image_prepare_ms` 记录从收到截图到完成解码与多屏拆分。`trigger_to_selection_ms` 记录从应用处理截图命令到 Qt 首次绘制选区。GNOME 的首帧显示还受合成器的呈现时机影响。

在嵌套 GNOME 48.7 和 Ubuntu 26.04 Portal 前端的三台 800×600 虚拟屏上，每种配置重复三次，保留全局动画。旧透明样式下，首个选区在命令触发后约 701 毫秒可见。启用提速扩展后，中位数约为 255 毫秒。测量包括截图命令转发和合成器显示时间。已核对 GNOME 49 和 50.1 的内部接口，物理 Ubuntu 26.04 多屏速度随分辨率、画面及硬件变化。

在三台 1920×1080 虚拟屏的高细节测试画面中，即使跳过闪光，GNOME 交付整张 PNG 仍用了约 0.7–0.9 秒。要让框选出现在 PNG 编码之前，需要在合成器里先持有冻结画面并显示选区。GNOME 自带的 `Print` 工具采用这种顺序，用户确认后才导出图片。非交互式 Screenshot Portal 在交出图片前没有可供 SnapLing 绘制的截图帧。

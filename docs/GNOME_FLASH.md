# 保留 GNOME 动画并缩短截图等待

GNOME 的单次截图 Portal 会等截图文件和闪光动画都完成，再把图片交给 SnapLing。此前的透明 CSS 只隐藏白色背景，约 500 毫秒的闪光淡出仍在运行。两个任务并行，因此缩短的时间取决于截图编码与动画的重叠程度。[GNOME 50 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/screenshotdialog.c#L247-L255) · [GNOME Shell 50.1 的截图和过渡](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/screenshot.js#L2668-L2684)

可选扩展只跳过 GNOME Shell 单次截图的闪光过渡，保留其他系统动画。扩展使用 GNOME Shell 的内部方法，版本声明限定在 GNOME 48 至 50。它适用于当前用户并作用于系统的单次截图。截图授权、取图和快门声仍由系统处理。

SnapLing 的 `.deb` 已附带提速扩展。安装后可以直接使用默认截图。需要提速时，打开 **设置 → 通用 → 单次截图提速**，点击 **启用**。首次安装后若 GNOME 尚未发现扩展，注销并重新登录一次即可启用。

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

截图仍需要取图、编码、解码和绘制。SnapLing 新增 `portal_round_trip_ms`（从发送截图请求到 Portal 返回）和 `image_prepare_ms`（从收到截图到完成解码与多屏拆分）。`trigger_to_selection_ms` 记录从应用处理截图命令到 Qt 首次绘制选区。已关闭全局动画时，这些数据可判断剩余等待出在哪一段。

在嵌套 GNOME 48.7 和 Ubuntu 26.04 Portal 前端的三台 800×600 虚拟屏上，每种配置重复三次，保留全局动画。旧透明样式下，首个选区在命令触发后约 701 毫秒可见。启用提速扩展后，中位数约为 255 毫秒。测量包括截图命令转发和合成器显示时间。已核对 GNOME 49 和 50.1 的内部接口，物理 Ubuntu 26.04 多屏速度随分辨率、画面及硬件变化。

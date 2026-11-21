# 保留 GNOME 动画并隐藏截图白闪

GNOME 的单次截图 Portal 没有关闭白闪的参数。GNOME 后端固定请求显示闪光，Shell 用 `.flashspot` 绘制白色背景。可以用一个只加载样式的用户扩展把这层背景改成透明，继续使用单次截图并保留其他 GNOME 动画。[GNOME 50 截图后端](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/screenshotdialog.c#L247-L255) · [GNOME Shell 50.1 样式](https://github.com/GNOME/gnome-shell/blob/50.1/data/theme/gnome-shell-sass/widgets/_misc.scss#L30)

在自己的 GNOME 会话中运行下面的命令，仅为当前用户创建扩展。

```bash
extension_id='snapling-no-flash@snapling.local'
extension_dir="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$extension_id"
mkdir -p -- "$extension_dir"
cat > "$extension_dir/metadata.json" <<'EOF'
{
  "uuid": "snapling-no-flash@snapling.local",
  "name": "Hide Screenshot Flash",
  "description": "Make the GNOME Shell screenshot flash transparent",
  "shell-version": ["48", "49", "50"]
}
EOF
cat > "$extension_dir/extension.js" <<'EOF'
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

export default class HideScreenshotFlash extends Extension {
    enable() {}
    disable() {}
}
EOF
printf '.flashspot { background-color: transparent; }\n' > "$extension_dir/stylesheet.css"
```

注销并重新登录一次，让 GNOME 发现新扩展。启用它，并把之前关闭的界面动画恢复为默认值。

```bash
gnome-extensions enable snapling-no-flash@snapling.local
gsettings reset org.gnome.desktop.interface enable-animations
```

这条样式会隐藏所有使用 GNOME Shell `.flashspot` 的截图白闪。截图过渡时间和快门声仍由系统处理。需要恢复白闪时运行 `gnome-extensions disable snapling-no-flash@snapling.local`。

验证使用 Ubuntu 26.04 的 Portal 前端和隔离 GNOME Shell 48.7。三台虚拟屏均以 60 帧每秒采样，保持动画打开时，加载样式前各有 17、15、14 个明显亮帧，加载后均为 0。Ubuntu 26.04 的 GNOME Shell 50.1 保留相同的样式类，尚未在物理屏幕验证该规则。

# 构建、安装与部署

## Ubuntu 24.04 / 26.04

最低 Qt 6.4，CMake 3.22，C++20。不需要 Electron、Node 或 Python 运行应用。Ubuntu 24.04 使用发行版 Qt 6.4.2；本次 Ubuntu 26.04 开发机使用 Qt 6.10.2。不要混用这两个版本的 Qt 插件。

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build file pkg-config libxkbcommon-dev qt6-base-dev qt6-wayland qt6-svg-dev \
  libtesseract-dev libpipewire-0.3-dev libx11-dev libxrandr-dev libxcb1-dev \
  libzxing-dev xvfb xauth xdotool fonts-noto-cjk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/snapling
```

模型已包含在 `models/tessdata`。开发环境可设置 `TESSDATA_PREFIX`，或在设置中手动选模型目录。Windows 多配置构建的测试已由 CTest 指定源码模型路径。

无管理员权限时：

```bash
python3 tools/bootstrap-local.py
source tools/dev-env.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./tools/run.sh
```

`.deps` 仅为当前发行版的开发前缀，软件包按 apt 元数据 SHA256 校验，不修改系统。Python 仅运行开发脚本。该模式可能引用主机已安装的同版本运行库；交付使用安装目录或便携包。

可选开关：`-DSNAPLING_PIPEWIRE=OFF` 保留 Portal 普通截图；`-DSNAPLING_TESSERACT=OFF` 编译不含 OCR 的诊断构建，不能当成完整版本交付。ZXing 缺失时二维码入口明确报告不可用。Linux 即使 Wayland 运行也需要 X11 开发库以构建 X11 兼容后端。

## Linux 安装

```bash
cmake --install build --prefix "$HOME/.local"
```

可执行文件安装至 `bin`，启动器至 `share/applications`，图标至 `share/icons`，模型至 `share/snapling/tessdata`。确保 `~/.local/bin` 在 PATH 中。系统安装可使用 `/usr/local` 前缀。

```bash
./tools/package-linux.sh
./dist/SnapLing/snapling
```

生成便携目录、tar.gz 和 DEB。便携入口设置相对库/插件/模型路径；动态库保持可替换，附带许可证。DEB 依赖目标发行版库，不内嵌 glibc。当前脚本的手动 DEB 依赖名按 Ubuntu 26.04 设置；在 Ubuntu 24.04 优先直接 `cd build && cpack -G DEB`，由 dpkg-shlibdeps 生成依赖。

**ABI 基线取决于构建机。** Ubuntu 26.04 包不承诺运行于 24.04；面向 24.04 的包应在 24.04 构建并再测试 26.04。AppImage 同样不能自动解决新 glibc 依赖。

AppImage 方案：从 [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy/releases) 与 [Qt 插件](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases) 的官方发行页下载匹配 x86_64 工具，检查发行方校验，将命令放到 PATH，运行 `packaging/linux/appimage.sh`。此脚本不自动执行未经确认的网络下载；本次是否实际生成 AppImage 以 TESTING.md 为准。

## Windows 10 / 11 x64

安装 Visual Studio 2022 的桌面 C++ 工作负载、CMake、Qt 6.4+ MSVC x64（CI 配置 6.5.3）、vcpkg 和可选 NSIS。

```powershell
vcpkg install tesseract:x64-windows
cmake -S . -B build -A x64 `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
.\build\Release\snapling.exe
.\packaging\windows\deploy.ps1 -QtBin C:/Qt/6.5.3/msvc2019_64/bin
```

`windeployqt` 部署 Qt 插件与编译器运行库，脚本再复制 vcpkg 的应用本地 DLL。输出 `dist/SnapLing-Windows-x64.zip`；移到没有 Qt 的干净机器验证。模型随安装放入 `share/snapling/tessdata`。

```powershell
cd packaging/windows
makensis installer.nsi
```

安装器采用当前用户目录，无需管理员权限，创建启动菜单入口和卸载项。卸载移除本应用登录启动项，保留用户主动保存的截图和配置。图标在 `resources/snapling.ico`，DPI manifest 及兼容 DPI 初始化在平台入口中。

Windows 源码与 CI 配置已提供；没有实际 Windows CI 执行记录前，不视为 Windows 构建验证完成。GitHub Actions 配置位于 `.github/workflows/build.yml`，需将项目放入有 Actions 权限的仓库后运行。

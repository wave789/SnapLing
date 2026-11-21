# 构建、安装与部署

## Ubuntu 24.04 / 26.04

最低 Qt 6.4，CMake 3.22，C++20。主程序和默认 Tesseract 不依赖 Python，可选 RapidOCR 使用独立的 Python 虚拟环境。Ubuntu 24.04 使用发行版 Qt 6.4.2，Ubuntu 26.04 使用 Qt 6.10.2。Qt 插件须匹配运行库版本。

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build file pkg-config libxkbcommon-dev qt6-base-dev qt6-wayland qt6-svg-dev \
  libtesseract-dev libpipewire-0.3-dev libx11-dev libxrandr-dev libxcb1-dev \
  libzxing-dev xvfb xauth xdotool fonts-noto-cjk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./tools/run.sh
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

`.deps` 仅为当前发行版的开发前缀，软件包按 apt 元数据 SHA256 校验。该模式可能引用主机已安装的同版本运行库，交付使用安装目录或便携包。

源码启动请使用 `tools/run.sh`。它在 Qt 初始化前自动写入当前用户的 `io.snapling.SnapLing.desktop`，供 Wayland Portal 查找应用身份；不需要先安装完整项目或使用 sudo。入口指向当前源码启动脚本的绝对路径，保留本地开发依赖环境。默认写入 `~/.local/share/applications`，遵循 `XDG_DATA_HOME`；准备失败会停止启动并报告路径。只需准备入口时可运行 `tools/install-desktop.sh`。直接执行 `build/snapling` 不执行这一步。

Wayland 默认使用非交互式 Screenshot Portal，支持单次完整桌面和多屏即时框选，不依赖 PipeWire。默认构建 `-DSNAPLING_PIPEWIRE=ON` 额外提供 ScreenCast 兼容截图，需要 `libpipewire-0.3-dev`；关闭此选项不影响默认单次截图。`-DSNAPLING_TESSERACT=OFF` 编译不含 Tesseract 的诊断构建，常规交付请开启此选项。ZXing 缺失时二维码入口明确报告不可用。Linux 即使 Wayland 运行也需要 X11 开发库以构建 X11 兼容后端。

## Linux 安装

编译完成后运行用户安装脚本。

```bash
./tools/install.sh
```

程序、资源和文档统一安装到 `~/.local/snapling`。脚本会把该目录内的桌面入口链接到 `${XDG_DATA_HOME:-$HOME/.local/share}/applications`，供应用菜单和截图 Portal 查找。可从应用菜单启动，或运行安装完成时打印的绝对路径。通过 `.deps` 构建时，脚本会一并收集运行库并在安装根目录生成 `snapling` 启动器。

重复执行可更新安装。安装脚本同时补齐 `~/.snapling/ocr/paddleocr` 下的 ONNX 与 YAML。若旧进程在运行，请从托盘退出后启动新版。可用 `./tools/install.sh --prefix /绝对路径` 更换用户安装目录。构建 DEB 或执行系统安装时，继续使用相应的打包脚本或 `cmake --install build --prefix /usr/local`。系统安装把模型资源放入前缀的 `share/snapling/models/paddleocr`，用户运行 `setup-rapidocr.py` 时复制到自己的模型目录。

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

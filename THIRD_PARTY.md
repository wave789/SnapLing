# 第三方依赖

本项目自身源码 MIT。下表是依赖及开发环境版本；依赖许可证独立于本项目许可证。Linux 动态库的发行版版权说明由便携打包脚本收集至 share/licenses，OCR 模型版权随源码提供。

| 组件 | 当前主机版本 | 用途 | 上游 / 许可证 |
|---|---|---|---|
| Qt Base / Widgets / Network / Concurrent / DBus / Test | 6.10.2；最低 6.4 | GUI、网络、线程、测试 | https://code.qt.io/cgit/qt/qtbase.git/ · LGPL-3.0/GPL 或商业；测试构建见 Qt 各组件条款 |
| Qt SVG | 6.10.2 | 图标插件、开发图标工具 | https://code.qt.io/cgit/qt/qtsvg.git/ · LGPL-3.0/GPL 或商业 |
| Qt Wayland | 6.10.2 | Wayland 平台插件 | https://code.qt.io/cgit/qt/qtwayland.git/ · 依组件 LGPL/GPL 或商业 |
| Tesseract | 5.5.0 | 本地 OCR | https://github.com/tesseract-ocr/tesseract · Apache-2.0 |
| Leptonica | 1.86.0 | Tesseract 依赖 | https://github.com/DanBloomberg/leptonica · BSD-2-Clause |
| tessdata_fast | Ubuntu 4.1.0-2build1 | 中英文模型 | https://github.com/tesseract-ocr/tessdata_fast · Apache-2.0 |
| Xlib / XRandR / XCB | 发行版 | X11 捕获、热键 | https://gitlab.freedesktop.org/xorg/lib · MIT/X11 |
| PipeWire / SPA | 1.6.2 | 授权后的帧读取 | https://gitlab.freedesktop.org/pipewire/pipewire · MIT |
| ZXing-C++ | 2.3.0 | 可选二维码 | https://github.com/zxing-cpp/zxing-cpp · Apache-2.0 |

可选的 RapidOCR 由用户运行安装脚本后从 PyPI 安装到独立环境，依赖 [RapidOCR](https://github.com/RapidAI/RapidOCR)（Apache-2.0）、[ONNX Runtime](https://github.com/microsoft/onnxruntime)（MIT）和 Python 包声明的依赖。默认发布包不包含这些依赖。编译器、平台库、字体和 OpenSSL 等传递依赖由构建系统和发行版提供。发布包须保留实际所含组件的完整许可证，允许接收者替换 LGPL 动态库。

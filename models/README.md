# 离线 OCR 模型

默认 OCR 引擎为 Tesseract 5 LSTM。Linux 可按项目 README 的步骤另外安装 RapidOCR，并在设置中切换。RapidOCR 的模型来自官方 Python 发行包，保存在 `~/.snapling/ocr/RapidOCR/models`。

可选的 PaddleOCR 配置使用项目 `src/assets/models/PaddleOCR` 下的 PP-OCRv6 Small 检测、识别和文字方向模型。Linux 用户安装将三份 ONNX 和对应的 `inference.yml` 分别复制到 `~/.snapling/ocr/paddleocr/{det,rec,cls}`。识别时从 YAML 读取文本检测阈值、方向预处理和识别字符表。两种 ONNX 模式共用 `setup-rapidocr.py` 准备的 CPU 运行环境。

随仓库部署的模型来自 Ubuntu 26.04 `tesseract-ocr-eng`、`tesseract-ocr-chi-sim` 软件包，版本 `1:4.1.0-2build1`；上游 [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast)。许可证 **Apache-2.0**，发行版版权记录见 `TESSDATA-LICENSE`，许可证正文见上游与发行版版权文件。

精确文件大小、版本、来源及 SHA256 在 `manifest.json`。

```bash
cd models
sha256sum -c SHA256SUMS
```

- `eng.traineddata`：4,113,088 bytes（实际以 manifest 为准），英文。
- `chi_sim.traineddata`：约 2.4 MiB，简体中文；与英文可组合 `chi_sim+eng`。

随附位置为 `<prefix>/share/snapling/tessdata` 或源码的 `models/tessdata`。首次自动识别时，SnapLing 将随附的 Tesseract 默认模型复制到 `~/.snapling/ocr/tesseract`，保留已有模型文件。显式选择的目录或有效的 `TESSDATA_PREFIX` 仍可使用。

手动导入支持完全离线部署：从可信上游/发行版取得相应 traineddata，核对下载方提供的 SHA256，在设置 → OCR 中选择目录；「查看模型大小与 SHA256」展示当前文件摘要。任意手动模型不会被误标成官方原始哈希一致，用户负责校验其可信来源。推理失败会报告错误，不会使用伪造结果。

Tesseract 在首次 OCR 时加载模型，默认可缓存，取消缓存则识别完成后释放。RapidOCR 在每次识别时按需启动，安装脚本会准备模型和依赖。

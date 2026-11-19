# 离线 OCR 模型

当前生产 OCR 使用 **Tesseract 5 LSTM**，不是 RapidOCR。`IOcrEngine` 可接入其他本地引擎，但仓库没有声称已实现 ONNX 推理。

随仓库部署的模型来自 Ubuntu 26.04 `tesseract-ocr-eng`、`tesseract-ocr-chi-sim` 软件包，版本 `1:4.1.0-2build1`；上游 [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast)。许可证 **Apache-2.0**，发行版版权记录见 `TESSDATA-LICENSE`，许可证正文见上游与发行版版权文件。

精确文件大小、版本、来源及 SHA256 在 `manifest.json`。

```bash
cd models
sha256sum -c SHA256SUMS
```

- `eng.traineddata`：4,113,088 bytes（实际以 manifest 为准），英文。
- `chi_sim.traineddata`：约 2.4 MiB，简体中文；与英文可组合 `chi_sim+eng`。

安装位置：`<prefix>/share/snapling/tessdata`。开发模式查找 `../models/tessdata`，Windows 也可放在程序旁的 `tessdata`。`TESSDATA_PREFIX` 与用户设置可覆盖路径。

手动导入支持完全离线部署：从可信上游/发行版取得相应 traineddata，核对下载方提供的 SHA256，在设置 → OCR 中选择目录；「查看模型大小与 SHA256」展示当前文件摘要。任意手动模型不会被误标成官方原始哈希一致，用户负责校验其可信来源。推理失败会报告错误，不会使用伪造结果。

模型只在 OCR 首次请求加载，默认可缓存，取消缓存则识别完成后释放。软件不自动下载模型、不自动运行后台 OCR、不启动大模型服务。

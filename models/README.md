# 离线 OCR 模型

SnapLing 的 OCR 固定使用 PaddleOCR PP-OCRv6 Small，源代码中的模型位于：

```text
src/assets/models/PaddleOCR/
  det/PP-OCRv6_small_det.onnx
  det/inference.yml
  rec/PP-OCRv6_small_rec.onnx
  rec/inference.yml
  cls/PP-OCR_text_angle.onnx
  cls/inference.yml
```

`.deb` 会把上述文件原样安装到 `/usr/share/snapling/models/paddleocr`，并把固定的 CPU 推理环境安装到 `/usr/share/snapling/paddleocr-runtime`。安装后不需要复制模型、选择模型目录或运行下载脚本。

构建时运行：

```bash
python3 tools/prepare-paddleocr-runtime.py --output build/paddleocr-runtime
```

该命令固定 RapidOCR `3.10.0` 与 ONNX Runtime `1.26.0`，生成的 `.runtime.json` 记录 Python ABI、工具版本和工作脚本 SHA256。相同运行时可复用；脚本或模型配置变化后重新运行即可更新。DEB 构建机需要联网一次，目标机器不需要。

`models/tessdata`、`SHA256SUMS` 和 `TESSDATA-LICENSE` 仅保留给显式启用 `-DSNAPLING_TESSERACT=ON` 的历史回归测试，不属于默认安装或正式 DEB。

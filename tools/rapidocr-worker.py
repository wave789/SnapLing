"""Private JSON protocol between SnapLing and its local RapidOCR process."""

from contextlib import redirect_stdout
from hashlib import file_digest
import importlib.metadata
import json
from pathlib import Path
import shutil
import sys
import tempfile


# Model files provided by the pinned official rapidocr wheel.
MODELS = {
    "Det": ("PP-OCRv6_det_small.onnx", "090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f"),
    "Cls": ("ch_ppocr_mobile_v2.0_cls_mobile.onnx", "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c"),
    "Rec": ("PP-OCRv6_rec_small.onnx", "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884"),
}


def install_models(destination):
    destination.mkdir(parents=True, exist_ok=True)
    distribution = importlib.metadata.distribution("rapidocr")
    for filename, expected in MODELS.values():
        target = destination / filename
        if target.is_file():
            continue
        source = distribution.locate_file("rapidocr/models/" + filename)
        with source.open("rb") as model:
            if file_digest(model, "sha256").hexdigest() != expected:
                raise RuntimeError(f"模型校验失败：{filename}")
            model.seek(0)
            with tempfile.NamedTemporaryFile(dir=destination, delete=False) as staged:
                try:
                    shutil.copyfileobj(model, staged)
                    staged.close()
                    # Publish a completed file without replacing a user's model.
                    try:
                        target.hardlink_to(staged.name)
                    except FileExistsError:
                        pass
                finally:
                    Path(staged.name).unlink(missing_ok=True)


def recognize(input_path):
    models = Path(__file__).resolve().parent / "models"
    params = {"Global.log_level": "error", "EngineConfig.onnxruntime.intra_op_num_threads": 2,
              "EngineConfig.onnxruntime.inter_op_num_threads": 1}
    for stage, (filename, _) in MODELS.items():
        path = models / filename
        if not path.is_file():
            raise RuntimeError(f"本地模型缺失：{path}。请重新运行 setup-rapidocr.py。")
        params[f"{stage}.model_path"] = str(path)

    # Keep the inference pipe machine-readable even if a dependency prints diagnostics.
    with redirect_stdout(sys.stderr):
        from rapidocr import RapidOCR
        from rapidocr.utils.download_file import DownloadFile

        def missing_resource(*args, **kwargs):
            raise RuntimeError("本地模型缺少所需资源。请重新运行 setup-rapidocr.py。")

        # The worker is offline even if a replacement model is missing a dictionary.
        DownloadFile.run = missing_resource
        result = RapidOCR(params=params)(input_path.read_bytes())

    if result.boxes is None:
        return []
    return [{"text": text, "box": box.tolist(), "confidence": float(score)}
            for text, box, score in zip(result.txts, result.boxes, result.scores, strict=True)]


def main(args):
    if len(args) == 2 and args[0] == "--install-models":
        install_models(Path(args[1]))
    elif len(args) == 1:
        print(json.dumps(recognize(Path(args[0])), ensure_ascii=False, allow_nan=False))
    else:
        raise ValueError("需要提供一张本地 PNG 图片")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)

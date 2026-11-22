"""Run the shipped PP-OCRv6 small model set using the pinned RapidOCR runtime."""

from contextlib import contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory

import cv2
import numpy as np
import yaml
from rapidocr import RapidOCR
from rapidocr.utils.typings import OCRVersion


MODELS = {
    "Det": "det/PP-OCRv6_small_det.onnx",
    "Cls": "cls/PP-OCR_text_angle.onnx",
    "Rec": "rec/PP-OCRv6_small_rec.onnx",
}


def transform(config, name):
    for operation in config["PreProcess"]["transform_ops"]:
        if name in operation:
            return operation[name]
    raise ValueError(f"PaddleOCR inference.yml 缺少 {name}")


@contextmanager
def make_engine(model_dir):
    params = {
        "Global.log_level": "error",
        "EngineConfig.onnxruntime.intra_op_num_threads": 2,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
        "Det.use_dilation": False,
        "Cls.ocr_version": OCRVersion.PPOCRV5,
    }
    configs = {}
    for stage, relative in MODELS.items():
        model = Path(model_dir) / relative
        config = model.with_name("inference.yml")
        for file in (model, config):
            if not file.is_file():
                raise RuntimeError(f"PaddleOCR 模型文件缺失：{file}。请重新安装 SnapLing。")
        with config.open(encoding="utf-8") as source:
            configs[stage] = yaml.safe_load(source)
        params[f"{stage}.model_path"] = str(model)

    det_norm = transform(configs["Det"], "NormalizeImage")
    params["Det.mean"] = det_norm["mean"]
    params["Det.std"] = det_norm["std"]
    det_post = configs["Det"]["PostProcess"]
    for key in ("thresh", "box_thresh", "max_candidates", "unclip_ratio"):
        params[f"Det.{key}"] = det_post[key]

    cls_config = configs["Cls"]
    cls_width, cls_height = transform(cls_config, "ResizeImage")["size"]
    cls_labels = cls_config["PostProcess"]["Topk"]["label_list"]
    if (cls_width, cls_height) != (160, 80) or cls_labels != ["0_degree", "180_degree"]:
        raise ValueError("PaddleOCR 文字方向模型的尺寸或标签配置不受支持")
    cls_norm = transform(cls_config, "NormalizeImage")
    cls_mean = np.asarray(cls_norm["mean"], dtype=np.float32).reshape(1, 1, 3)
    cls_std = np.asarray(cls_norm["std"], dtype=np.float32).reshape(1, 1, 3)
    cls_scale = float(cls_norm["scale"])
    params["Cls.label_list"] = cls_labels

    rec_shape = transform(configs["Rec"], "RecResizeImg")["image_shape"]
    if rec_shape[:2] != [3, 48] or rec_shape[2] <= 0:
        raise ValueError("PaddleOCR 文字识别模型的输入尺寸不受支持")
    params["Rec.rec_img_shape"] = rec_shape
    rec_post = configs["Rec"]["PostProcess"]
    dictionary = rec_post["character_dict"]
    if (rec_post["name"] != "CTCLabelDecode" or not dictionary or
            any(not isinstance(char, str) or len(char) != 1 or char in " \r\n" for char in dictionary) or
            len(set(dictionary)) != len(dictionary)):
        raise ValueError("PaddleOCR 文字识别模型的 CTC 字典无效")

    def normalize_orientation(image):
        # PP-LCNet textline orientation uses RGB, stretching, and ImageNet normalization.
        # RapidOCR 3.10 expects the same tensor size for PPOCRV5 but normalizes differently.
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        resized = cv2.resize(rgb, (cls_width, cls_height), interpolation=cv2.INTER_LINEAR)
        normalized = (resized.astype(np.float32) * cls_scale - cls_mean) / cls_std
        return normalized.transpose(2, 0, 1)

    with TemporaryDirectory(prefix="snapling-paddleocr-") as temporary:
        # The uploaded recognition ONNX has no character metadata. RapidOCR inserts CTC
        # blank and space around the YAML dictionary when it reads this list.
        vocab = Path(temporary) / "characters.txt"
        vocab.write_text("\n".join(dictionary) + "\n", encoding="utf-8")
        params["Rec.rec_keys_path"] = str(vocab)

        engine = RapidOCR(params=params)
        # These hooks are from the RapidOCR version pinned by prepare-paddleocr-runtime.py.
        classifier = engine._load_cls_model()
        cls_session = classifier.session.session
        if (cls_session.get_inputs()[0].shape[1:] != [3, cls_height, cls_width] or
                cls_session.get_outputs()[0].shape[-1] != len(cls_labels)):
            raise ValueError("PaddleOCR 文字方向 ONNX 与 inference.yml 不匹配")
        classifier.resize_norm_img = normalize_orientation

        rec_session = engine._load_rec_model().session.session
        if (rec_session.get_inputs()[0].shape[1:3] != rec_shape[:2] or
                rec_session.get_outputs()[0].shape[-1] != len(dictionary) + 2):
            raise ValueError("PaddleOCR 文字识别 ONNX 与 inference.yml 字典或尺寸不匹配")
        yield engine

#!/usr/bin/env python3
"""Install the optional RapidOCR CPU engine in SnapLing's user model directory."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


RAPIDOCR_VERSION = "3.10.0"
ONNXRUNTIME_VERSION = "1.26.0"
WORKER = "rapidocr-worker.py"


def default_model_root():
    configured = Path(os.environ.get("SNAPLING_OCR_HOME", ""))
    if configured.is_absolute():
        return configured
    return Path.home() / ".snapling" / "ocr"


def replace_file(path, contents, mode):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as file:
        staged = Path(file.name)
        try:
            file.write(contents)
            file.close()
            staged.chmod(mode)
            staged.replace(path)
        finally:
            staged.unlink(missing_ok=True)


def install(root):
    venv = root / f".venv-rapidocr-{RAPIDOCR_VERSION}-ort-{ONNXRUNTIME_VERSION}"
    python = venv / "bin" / "python"
    if not python.is_file():
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)

    pip_environment = dict(os.environ, PIP_CONFIG_FILE=os.devnull)
    subprocess.run(
        [str(python), "-I", "-m", "pip", "--isolated", "--no-input", "--disable-pip-version-check", "install",
         "--no-cache-dir", "--only-binary=:all:", "--index-url", "https://pypi.org/simple",
         f"rapidocr=={RAPIDOCR_VERSION}", f"onnxruntime=={ONNXRUNTIME_VERSION}"],
        check=True,
        env=pip_environment,
    )
    worker = Path(__file__).resolve().with_name(WORKER)
    subprocess.run([str(python), "-I", str(worker), "--install-models", str(root / "models")], check=True)
    replace_file(root / WORKER, worker.read_bytes(), 0o600)
    # exec makes the inference process directly cancellable by the application.
    launcher = (
        '#!/bin/sh\n'
        'root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
        f'exec "$root/{venv.name}/bin/python" -I "$root/{WORKER}" "$@"\n'
    )
    replace_file(root / "run-ocr", launcher.encode("utf-8"), 0o700)
    print(f"RapidOCR 已准备完成。模型目录为 {root / 'models'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=default_model_root() / "RapidOCR",
                        help="安装目录，默认为 ~/.snapling/ocr/RapidOCR")
    args = parser.parse_args()
    if sys.platform != "linux" or sys.version_info < (3, 11):
        parser.exit(1, "此安装脚本需要 Linux 和 Python 3.11 或更新版本。\n")
    import fcntl

    try:
        root = args.root.expanduser().resolve()
        root.mkdir(parents=True, exist_ok=True)
        # Serialize repeated setup so concurrent pip runs cannot change the same venv.
        with (root / ".setup.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            install(root)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"RapidOCR 安装失败。请确认已安装 python3-venv，并能访问 PyPI。\n{error}\n")


if __name__ == "__main__":
    main()

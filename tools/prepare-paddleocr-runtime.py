#!/usr/bin/env python3
"""Build the relocatable PaddleOCR runtime shipped inside SnapLing packages."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


RAPIDOCR_VERSION = "3.10.0"
ONNXRUNTIME_VERSION = "1.26.0"
TOOLS = Path(__file__).resolve().parent
FILES = ("rapidocr-worker.py", "paddleocr_profile.py")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def current_marker(output, python):
    if not python.is_file():
        return None
    report = subprocess.run(
        [str(python), "-c", "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return {
        "rapidocr": RAPIDOCR_VERSION,
        "onnxruntime": ONNXRUNTIME_VERSION,
        "python": report,
        "builder": digest(Path(__file__).resolve()),
        "files": {name: digest(TOOLS / name) for name in FILES},
    }


def launcher(name, flag):
    return (
        "#!/bin/sh\n"
        'root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
        f'exec "$root/bin/python3" -I "$root/rapidocr-worker.py"'
        f'{flag} "$@"\n'
    )


def prune_runtime(staging):
    site_packages = next(staging.glob("lib/python*/site-packages"), None)
    if site_packages is None:
        raise RuntimeError("PaddleOCR runtime has no site-packages directory")
    for name in ("pip", "setuptools", "wheel", "pkg_resources"):
        path = site_packages / name
        if path.exists():
            shutil.rmtree(path)
    for path in list(site_packages.glob("pip-*.dist-info")) + list(site_packages.glob("setuptools-*.dist-info")) + \
            list(site_packages.glob("wheel-*.dist-info")):
        shutil.rmtree(path)
    # Only the shipped Paddle models are passed to the worker; default RapidOCR
    # model copies and inference-adjacent tooling are not needed at runtime.
    for relative in (
        "rapidocr/models",
        "onnxruntime/tools",
        "onnxruntime/transformers",
        "onnxruntime/quantization",
        "shapely/tests",
    ):
        path = site_packages / relative
        if path.exists():
            shutil.rmtree(path)
    for tests in site_packages.rglob("tests"):
        if tests.is_dir() and ("numpy" in tests.parts or tests.parent.name == "shapely"):
            shutil.rmtree(tests, ignore_errors=True)
    for cache in site_packages.rglob("__pycache__"):
        shutil.rmtree(cache, ignore_errors=True)
    runtime_models = staging / "models"
    if runtime_models.exists():
        shutil.rmtree(runtime_models)
    bin_dir = staging / "bin"
    for path in bin_dir.iterdir():
        if path.name.startswith("python"):
            continue
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)


def prepare(output, python, force):
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    marker_path = output / ".runtime.json"
    expected = current_marker(output, python)
    if not force and marker_path.is_file() and expected:
        try:
            if json.loads(marker_path.read_text(encoding="utf-8")) == expected:
                print(f"PaddleOCR runtime is ready: {output}")
                return
        except (OSError, ValueError):
            pass

    staging = Path(tempfile.mkdtemp(prefix=".paddleocr-runtime-", dir=output.parent))
    try:
        try:
            subprocess.run([str(python), "-m", "venv", str(staging)], check=True)
        except subprocess.CalledProcessError:
            uv = shutil.which("uv")
            if not uv:
                raise RuntimeError(
                    "Python venv/ensurepip is unavailable. Install python3-venv or uv on the build machine."
                )
            subprocess.run(
                [uv, "venv", "--seed", "--clear", "--python", str(python), str(staging)],
                check=True,
            )
        runtime_python = staging / "bin" / "python3"
        environment = dict(os.environ, PIP_CONFIG_FILE=os.devnull)
        subprocess.run(
            [
                str(runtime_python),
                "-I",
                "-m",
                "pip",
                "--isolated",
                "--no-input",
                "--disable-pip-version-check",
                "install",
                "--no-cache-dir",
                "--only-binary=:all:",
                "--index-url",
                "https://pypi.org/simple",
                f"rapidocr=={RAPIDOCR_VERSION}",
                f"onnxruntime=={ONNXRUNTIME_VERSION}",
            ],
            check=True,
            env=environment,
        )
        for name in FILES:
            shutil.copy2(TOOLS / name, staging / name)
        prune_runtime(staging)
        for name, flag in (("run-ocr", ""), ("run-paddle-ocr", " --paddleocr")):
            path = staging / name
            path.write_text(launcher(name, flag), encoding="utf-8")
            path.chmod(0o755)
        marker = current_marker(staging, runtime_python)
        if marker is None:
            raise RuntimeError("PaddleOCR runtime did not expose its Python interpreter")
        (staging / ".runtime.json").write_text(
            json.dumps(marker, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if output.exists():
            shutil.rmtree(output)
        staging.replace(output)
        print(f"Prepared bundled PaddleOCR runtime: {output}")
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "build" / "paddleocr-runtime",
    )
    parser.add_argument("--python", type=Path, default=Path("/usr/bin/python3"))
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("DEB PaddleOCR runtime can only be prepared on Linux")
    if not args.python.is_file():
        parser.error(f"Python interpreter not found: {args.python}")
    try:
        prepare(args.output, args.python, args.force)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        parser.exit(1, f"Unable to prepare the bundled PaddleOCR runtime:\n{error}\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# Provision the bundled model files for the current user without replacing custom files.
set -euo pipefail
script_dir="$(cd "$(dirname "$0")" && pwd)"
if (( $# > 1 )); then
    printf '用法: %s [模型目录]\n' "$0" >&2
    exit 1
fi
model_source="${1:-$script_dir/../src/assets/models/PaddleOCR}"
if (( $# == 0 )) && [[ ! -d "$model_source" ]]; then
    model_source="$script_dir/../models/paddleocr"
fi
ocr_root="${SNAPLING_OCR_HOME:-}"
if [[ "$ocr_root" != /* ]]; then ocr_root="$HOME/.snapling/ocr"; fi
model_dir="$ocr_root/paddleocr"

model_files=(
    cls/PP-OCR_text_angle.onnx
    cls/inference.yml
    det/PP-OCRv6_small_det.onnx
    det/inference.yml
    rec/PP-OCRv6_small_rec.onnx
    rec/inference.yml
)
for name in "${model_files[@]}"; do
    source_file="$model_source/$name"
    if [[ ! -f "$source_file" || ! -r "$source_file" || ! -s "$source_file" ]]; then
        printf '缺少随附的 PaddleOCR 模型文件: %s\n' "$source_file" >&2
        exit 1
    fi
done

temp_file=""
trap 'if [[ -n "$temp_file" ]]; then rm -f -- "$temp_file"; fi' EXIT
for name in "${model_files[@]}"; do
    target="$model_dir/$name"
    if [[ ! -e "$target" && ! -L "$target" ]]; then
        mkdir -p -- "${target%/*}"
        temp_file="$(mktemp --tmpdir="${target%/*}" .snapling-model.XXXXXXXX)"
        cp -- "$model_source/$name" "$temp_file"
        # Publishing the complete file with a hard link never replaces another installation's copy.
        if ! ln -T -- "$temp_file" "$target" 2>/dev/null && [[ ! -e "$target" && ! -L "$target" ]]; then
            printf '无法安装 PaddleOCR 模型文件: %s\n' "$target" >&2
            exit 1
        fi
        rm -- "$temp_file"
        temp_file=""
    fi
    if [[ ! -f "$target" || ! -r "$target" || ! -s "$target" ]]; then
        printf '无法读取已有的 PaddleOCR 模型文件: %s\n' "$target" >&2
        exit 1
    fi
done
printf 'PaddleOCR 模型已准备到 %s\n' "$model_dir"

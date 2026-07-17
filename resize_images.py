#!/usr/bin/env python3
"""Resize images in a folder when their largest dimension is below a target size."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterator

from PIL import Image, UnidentifiedImageError

SUPPORTED_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff", ".heic", ".heif"}


def iter_images(folder: Path) -> Iterator[Path]:
    for path in sorted(folder.iterdir()):
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS:
            yield path


def resize_image(image_path: Path, output_path: Path, target_max: int) -> bool:
    with Image.open(image_path) as img:
        width, height = img.size
        largest_dimension = max(width, height)
        print(f"Inspecting {image_path.name}: {width}x{height}, largest dimension: {largest_dimension}px, target max: {target_max}px")
        if largest_dimension <= target_max:
            return False

        scale = target_max / largest_dimension
        new_size = (max(1, int(width * scale)), max(1, int(height * scale)))

        if img.mode in {"RGBA", "LA", "P"}:
            converted = img.convert("RGBA")
        else:
            converted = img.convert("RGB")

        resized = converted.resize(new_size, resample=Image.Resampling.LANCZOS)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        resized.save(output_path)
        return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Resize images whose largest dimension is smaller than a target size.")
    parser.add_argument("input_folder", type=Path, help="Folder containing images to inspect", default="C:/temp/IcelandTripSmall/")
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        help="Directory where resized files are written (default: <input_folder>/resized)",
    )
    parser.add_argument("--target-max", type=int, default=1920, help="Target maximum dimension in pixels (default: 1920)")
    args = parser.parse_args()

    input_folder = args.input_folder.resolve()
    if not input_folder.exists() or not input_folder.is_dir():
        raise SystemExit(f"Input folder does not exist or is not a directory: {input_folder}")

    output_dir = (args.output_dir or input_folder / "resized").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    processed = 0
    skipped = 0
    errors = 0

    for image_path in iter_images(input_folder):
        output_name = f"{image_path.stem}_resized{image_path.suffix}"
        output_path = output_dir / output_name

        try:
            resized = resize_image(image_path, output_path, args.target_max)
        except (UnidentifiedImageError, OSError) as exc:
            print(f"Skipping {image_path.name}: {exc}")
            errors += 1
            continue

        if resized:
            print(f"Resized {image_path.name} -> {output_path}")
            processed += 1
        else:
            print(f"Skipped {image_path.name} (larger than or equal to {args.target_max}px)")
            skipped += 1

    print(f"Done. Resized: {processed}, skipped: {skipped}, errors: {errors}")


if __name__ == "__main__":
    main()

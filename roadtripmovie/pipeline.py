"""End-to-end orchestration: scan media, build segments, concatenate, mix music, export."""

from __future__ import annotations

import sys
from pathlib import Path

from . import video_builder
from .media_scanner import scan_media


def run(
    input_folder: Path,
    music_path: Path,
    output_path: Path,
    photo_duration: float = 5.0,
    resolution: tuple[int, int] = (1920, 1080),
    fps: int = 30,
    map_zoom: int = 14,
    map_size: tuple[int, int] = (240, 160),
    video_audio_volume: float = 0.25,
    music_volume: float = 1.0,
    max_video_duration: float | None = None,
    cache_dir: Path | None = None,
    show_map: bool = True,
):
    items = scan_media(input_folder)
    if not items:
        raise SystemExit(f"No supported photos/videos found in {input_folder}")

    if cache_dir is None:
        cache_dir = output_path.parent / ".map_cache"

    target_w, target_h = resolution
    map_w, map_h = map_size

    segments = []
    for item in items:
        print(f"Processing {item.path.name} ({item.kind}, {item.captured_at})...")
        try:
            segment = video_builder.build_segment(
                item=item,
                photo_duration=photo_duration,
                target_w=target_w,
                target_h=target_h,
                map_cache_dir=cache_dir,
                video_audio_volume=video_audio_volume,
                map_zoom=map_zoom,
                map_width=map_w,
                map_height=map_h,
                max_video_duration=max_video_duration,
                show_map=show_map,
            )
        except Exception as exc:
            print(f"Warning: skipping unreadable file {item.path}: {exc}", file=sys.stderr)
            continue
        segments.append(segment)

    if not segments:
        raise SystemExit("No media files could be processed successfully.")

    print("Concatenating segments...")
    final = video_builder.concatenate_all(segments)

    print("Mixing background music...")
    final = video_builder.mix_background_music(final, str(music_path), music_volume)

    print(f"Writing {output_path}...")
    final.write_videofile(str(output_path), fps=fps, codec="libx264", audio_codec="aac")

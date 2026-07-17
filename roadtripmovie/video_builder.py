"""Build per-item video segments, composite the map inset, concatenate, and mix in background music."""

from pathlib import Path
from typing import Optional

import numpy as np
from moviepy import (
    AudioFileClip,
    CompositeAudioClip,
    CompositeVideoClip,
    ImageClip,
    VideoFileClip,
    concatenate_videoclips,
)
from moviepy.audio.fx import AudioLoop
from PIL import Image, ImageOps

from . import map_thumbnail
from .media_scanner import MediaItem

MAP_MARGIN = 20


def _load_photo_array(path: str, target_w: int, target_h: int) -> np.ndarray:
    """Open and downscale a photo to fit the output canvas before it ever reaches moviepy.

    Real photos (12MP+ from a phone/camera) are far larger than the output resolution.
    ImageClip keeps whatever array it's given alive for the life of the pipeline run, so
    handing it the full-resolution image for every photo in a large trip folder can exhaust
    memory well before encoding starts. Resizing here means only a small, canvas-sized array
    is ever retained.
    """
    with Image.open(path) as img:
        img = ImageOps.exif_transpose(img)  # respect phone/camera orientation metadata
        img = img.convert("RGB")
        scale = min(target_w / img.width, target_h / img.height)
        new_size = (max(1, round(img.width * scale)), max(1, round(img.height * scale)))
        img = img.resize(new_size, Image.LANCZOS)
        return np.array(img)


def _fit_to_canvas(clip, target_w: int, target_h: int):
    """Resize a clip to fit within (target_w, target_h) preserving aspect ratio, centered on a black canvas."""
    scale = min(target_w / clip.w, target_h / clip.h)
    resized = clip.resized(scale).with_position("center")
    return CompositeVideoClip([resized], size=(target_w, target_h), bg_color=(0, 0, 0))


def _map_overlay_clip(
    item: MediaItem,
    duration: float,
    target_w: int,
    map_cache_dir: Path,
    map_width: int,
    map_height: int,
    zoom: int,
):
    lat, lon = item.location
    map_image = map_thumbnail.render_map(
        lat, lon, cache_dir=map_cache_dir, width=map_width, height=map_height, zoom=zoom
    )
    overlay = ImageClip(np.array(map_image)).with_duration(duration)
    return overlay.with_position((target_w - map_width - MAP_MARGIN, MAP_MARGIN))


def build_segment(
    item: MediaItem,
    photo_duration: float,
    target_w: int,
    target_h: int,
    map_cache_dir: Path,
    video_audio_volume: float,
    map_zoom: int,
    map_width: int,
    map_height: int,
    max_video_duration: Optional[float],
    show_map: bool,
):
    """Build a single composited, canvas-fitted segment (with optional map inset) for one media item."""
    if item.kind == "photo":
        array = _load_photo_array(str(item.path), target_w, target_h)
        base = ImageClip(array).with_duration(photo_duration)
    else:
        base = VideoFileClip(str(item.path))
        if max_video_duration is not None and base.duration > max_video_duration:
            base = base.subclipped(0, max_video_duration)
        if base.audio is not None:
            base = base.with_audio(base.audio.with_volume_scaled(video_audio_volume))

    segment = _fit_to_canvas(base, target_w, target_h)

    if show_map and item.location is not None:
        overlay = _map_overlay_clip(
            item, segment.duration, target_w, map_cache_dir, map_width, map_height, map_zoom
        )
        segment = CompositeVideoClip([segment, overlay], size=(target_w, target_h))

    return segment


def concatenate_all(segments: list):
    return concatenate_videoclips(segments, method="compose")


def mix_background_music(video_clip, music_path: str, music_volume: float):
    """Loop/trim background music to the video's total duration and mix with any existing segment audio."""
    total_duration = video_clip.duration
    music = AudioFileClip(music_path).with_effects([AudioLoop(duration=total_duration)])
    music = music.subclipped(0, total_duration).with_volume_scaled(music_volume)

    if video_clip.audio is not None:
        mixed = CompositeAudioClip([music, video_clip.audio])
    else:
        mixed = music

    return video_clip.with_audio(mixed)

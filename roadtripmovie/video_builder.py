"""Build per-item video segments, composite the map inset, write each to its own file, then
concatenate the files on disk and mix in background music.

Segments are written to disk one at a time (instead of being kept open as clip objects in a
list) so that a large trip folder never requires holding every photo/video in memory or as an
open decoder at once - only the single segment currently being processed.
"""

import subprocess
from pathlib import Path
from typing import Optional

import imageio_ffmpeg
import numpy as np
from moviepy import (
    AudioClip,
    AudioFileClip,
    CompositeAudioClip,
    CompositeVideoClip,
    ImageClip,
    VideoFileClip,
)
from moviepy.audio.fx import AudioLoop
from PIL import Image, ImageOps

from . import map_thumbnail
from .media_scanner import MediaItem

MAP_MARGIN = 20
AUDIO_FPS = 44100


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


def _silent_audio_clip(duration: float, fps: int = AUDIO_FPS) -> AudioClip:
    """A silent stereo audio track, used so every segment file has an audio stream.

    ffmpeg's concat demuxer (used to join segment files on disk) requires every input to have
    the same stream layout, so photo segments (which have no audio) need a silent placeholder
    to line up with video segments that do.
    """

    def make_frame(t):
        if np.isscalar(t):
            return np.zeros(2)
        return np.zeros((len(t), 2))

    return AudioClip(make_frame, duration=duration, fps=fps)


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

    if segment.audio is None:
        segment = segment.with_audio(_silent_audio_clip(segment.duration))

    return segment


def write_segment(segment, output_path: Path, fps: int) -> None:
    """Render one segment to its own file, so its frames/audio never have to coexist with any other segment's."""
    segment.write_videofile(
        str(output_path),
        fps=fps,
        codec="libx264",
        audio_codec="aac",
        audio_fps=AUDIO_FPS,
        logger=None,
    )
    segment.close()


def concatenate_video_files(file_paths: list[Path], output_path: Path) -> None:
    """Join already-rendered segment files with ffmpeg's concat demuxer (stream copy, no re-encode).

    Unlike concatenate_videoclips, this never has more than one segment's worth of encoded frames
    open at a time - ffmpeg streams the existing files straight into the joined output.
    """
    list_file = output_path.with_suffix(".txt")
    with open(list_file, "w", encoding="utf-8") as f:
        for path in file_paths:
            escaped = path.resolve().as_posix().replace("'", "'\\''")
            f.write(f"file '{escaped}'\n")

    try:
        ffmpeg_exe = imageio_ffmpeg.get_ffmpeg_exe()
        subprocess.run(
            [
                ffmpeg_exe,
                "-y",
                "-f", "concat",
                "-safe", "0",
                "-i", str(list_file),
                "-c", "copy",
                str(output_path),
            ],
            check=True,
            capture_output=True,
        )
    finally:
        list_file.unlink(missing_ok=True)


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

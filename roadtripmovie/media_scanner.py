"""Scan a folder for photos and videos, classify them, and order them chronologically."""

import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

from . import gps

PHOTO_EXTS = {".jpg", ".jpeg", ".png", ".heic", ".heif"}
VIDEO_EXTS = {".mp4", ".mov", ".avi", ".mkv", ".m4v"}


@dataclass
class MediaItem:
    path: Path
    kind: str  # "photo" or "video"
    captured_at: datetime
    location: Optional[tuple[float, float]]


def _resolve_item(path: Path) -> Optional[MediaItem]:
    ext = path.suffix.lower()
    if ext in PHOTO_EXTS:
        kind = "photo"
        location, captured_at = gps.get_photo_location_and_time(path)
    elif ext in VIDEO_EXTS:
        kind = "video"
        location, captured_at = gps.get_video_location_and_time(path)
    else:
        return None

    if captured_at is None:
        captured_at = datetime.fromtimestamp(path.stat().st_mtime)

    return MediaItem(path=path, kind=kind, captured_at=captured_at, location=location)


def scan_media(folder: Path) -> list[MediaItem]:
    """Walk a folder for supported photos/videos, returning items sorted by capture time."""
    items: list[MediaItem] = []
    for path in sorted(folder.iterdir()):
        if not path.is_file():
            continue
        try:
            item = _resolve_item(path)
        except Exception as exc:
            print(f"Warning: skipping unreadable file {path}: {exc}", file=sys.stderr)
            continue
        if item is None:
            continue
        items.append(item)

    items.sort(key=lambda item: item.captured_at)
    return items

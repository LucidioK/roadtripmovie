"""Scan a folder for photos and videos, classify them, and order them chronologically."""

import sys
from datetime import datetime
from pathlib import Path
import threading
from typing import Optional

from . import gps

PHOTO_EXTS = {".jpg", ".jpeg", ".png", ".heic", ".heif"}
VIDEO_EXTS = {".mp4", ".mov", ".avi", ".mkv", ".m4v"}


class MediaItem:
    """
    Represents a single media item (photo or video) with its metadata.
    Attributes:
        path (Path): The file path of the media item.
        kind (str): The type of media item ("photo" or "video").
        captured_at (datetime): The timestamp when the media item was captured.
        location (tuple[float, float]): The geographical coordinates (latitude, longitude)
            of the media item.
        city_name (str): The name of the city where the media item was captured.
        index (int): The index of the media item in the ordered list.
        lock (threading.Lock): A lock for thread-safe operations on this item.
    """
    def __init__(
        self,
        path: Path,
        kind: str,
        captured_at: datetime,
        location: tuple[float, float],
        city_name: str = None,
        index: int = 0,
        lock: threading.Lock = None,
    ):
        self.path = path
        self.kind = kind
        self.captured_at = captured_at
        self.location = location or [0.0, 0.0]
        self.city_name = city_name or ""
        self.index = index
        self.lock = lock or threading.Lock()

    def __getstate__(self) -> dict:
        state = self.__dict__.copy()
        del state["lock"]  # exclude unpicklable object
        return state

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

    city_name = gps.get_city_name(location[0], location[1]) if location else None
    return MediaItem(
        path=path,
        kind=kind,
        captured_at=captured_at,
        location=location,
        city_name=city_name,
    )


def scan_media(folder: Path) -> list[MediaItem]:
    """Walk a folder for supported photos/videos, returning items sorted by capture time."""
    items: list[MediaItem] = []
    for path in sorted(folder.iterdir()):
        if not path.is_file():
            continue
        try:
            item = _resolve_item(path)
        except (OSError, ValueError) as exc:
            print(
                f"Warning: scan_media skipping unreadable file {path}: {exc}\n{exc.__traceback__}",
                file=sys.stderr,
            )
            continue
        if item is None:
            continue
        items.append(item)

    items.sort(key=lambda item: item.captured_at)
    for index, item in enumerate(items):
        item.index = index
    return items

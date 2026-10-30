"""GPS coordinate and capture-time extraction for photos and videos."""

import json
import re
import shutil
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Optional
import numpy as np
import pandas as pd
import joblib
# from geopy.geocoders import Nominatim
# from geopy.exc import GeocoderTimedOut, GeocoderServiceError, GeocoderRateLimited

import imageio_ffmpeg
from PIL import ExifTags, Image

import pillow_heif

pillow_heif.register_heif_opener()

LatLon = tuple[float, float]

_ISO6709_RE = re.compile(r"^([+-]\d+\.?\d*)([+-]\d+\.?\d*)")
# Initialize Nominatim API (Always use a custom user_agent string)
# _GEOLOCATOR = Nominatim(user_agent="my_geo_app")
_CITY_SPATIAL_TREE = joblib.load("spatial_index.pkl")
_DF_CITIES = pd.read_feather("cities_metadata.feather")

def get_city_name(lat: float, lon: float) -> str:
    """
    Get the city name for given latitude and longitude 

    Args:
        lat (float): Latitude
        lon (float): Longitude

    Returns:
        str: City name, or an empty string if not found.
    """
    if -0.1 <= lat <= 0.1 and -0.1 <= lon <= 0.1:
        return "_"
    target_rad = np.deg2rad([[lat, lon]])
    _, index = _CITY_SPATIAL_TREE.query(target_rad, k=1)
    # print(f"{lat},{lon} Closest City: {_DF_CITIES.iloc[index[0][0]]['cityname']}")
    return _DF_CITIES.iloc[index[0][0]]['cityname']


def _get_city_name(lat: float, lon: float) -> str:
    """
    Get the city name for given latitude and longitude using the Nominatim API.

    Args:
        lat (float): Latitude
        lon (float): Longitude

    Returns:
        str: City name, or an empty string if not found.
    """
    
    try:
        # Query the coordinates (can accept a tuple or a string)
        location = _GEOLOCATOR.reverse((lat, lon), timeout=10)

        if location and "address" in location.raw:
            address = location.raw["address"]

            # OpenStreetMap uses different keys depending on the region's layout
            city = (
                address.get("city")
                or address.get("town")
                or address.get("village")
                or address.get("suburb")
            )
            return city
        return "_"
    except GeocoderRateLimited as e:
        print(f"GeocoderRateLimited ({lat}, {lon}) {e}")
        return "__"        
    except Exception as e:
        print(f"Error retrieving city name for coordinates ({lat}, {lon}) {e}")
        return "__"


def _dms_to_dd(dms, ref: str) -> float:
    degrees, minutes, seconds = dms
    dd = float(degrees) + float(minutes) / 60 + float(seconds) / 3600
    if ref in ("S", "W"):
        dd = -dd
    return dd


def get_photo_location_and_time(
    path: Path,
) -> tuple[Optional[LatLon], Optional[datetime]]:
    """Read GPS coordinates and DateTimeOriginal from a photo's EXIF data."""
    try:
        with Image.open(path) as img:
            exif = img.getexif()
            if not exif:
                return None, None

            location = None
            gps_ifd = exif.get_ifd(ExifTags.IFD.GPSInfo)
            if gps_ifd:
                try:
                    lat = _dms_to_dd(gps_ifd[2], gps_ifd[1])
                    lon = _dms_to_dd(gps_ifd[4], gps_ifd[3])
                    location = (lat, lon)
                except (KeyError, ValueError, TypeError, ZeroDivisionError):
                    location = None

            captured_at = None
            exif_sub_ifd = exif.get_ifd(ExifTags.IFD.Exif)
            date_str = (exif_sub_ifd or {}).get(36867) or exif.get(  # DateTimeOriginal
                306
            )  # DateTime (top-level IFD0, if present)
            if date_str:
                try:
                    captured_at = datetime.strptime(date_str, "%Y:%m:%d %H:%M:%S")
                except ValueError:
                    captured_at = None

            return location, captured_at
    except Exception:
        return None, None


def _parse_iso6709(value: str) -> Optional[LatLon]:
    match = _ISO6709_RE.match(value.strip())
    if not match:
        return None
    try:
        return float(match.group(1)), float(match.group(2))
    except ValueError:
        return None


def _read_tags_via_ffprobe(path: Path) -> Optional[dict]:
    ffprobe_exe = shutil.which("ffprobe")
    if not ffprobe_exe:
        return None
    try:
        result = subprocess.run(
            [
                ffprobe_exe,
                "-v",
                "quiet",
                "-print_format",
                "json",
                "-show_format",
                str(path),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        data = json.loads(result.stdout or "{}")
    except (subprocess.SubprocessError, json.JSONDecodeError, OSError):
        return None
    return data.get("format", {}).get("tags", {}) or {}


def _read_tags_via_ffmpeg_metadata(path: Path) -> dict:
    """Fallback for machines with only the imageio-ffmpeg-bundled ffmpeg (no ffprobe)."""
    ffmpeg_exe = imageio_ffmpeg.get_ffmpeg_exe()
    try:
        result = subprocess.run(
            [ffmpeg_exe, "-i", str(path), "-f", "ffmetadata", "-"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (subprocess.SubprocessError, OSError):
        return {}

    tags = {}
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith(";"):
            key, _, value = line.partition("=")
            tags[key.strip()] = value.strip()
    return tags


def get_video_location_and_time(
    path: Path,
) -> tuple[Optional[LatLon], Optional[datetime]]:
    """Read GPS coordinates and creation time from a video's container metadata."""
    tags = _read_tags_via_ffprobe(path)
    if tags is None:
        tags = _read_tags_via_ffmpeg_metadata(path)

    location = None
    for key in ("com.apple.quicktime.location.ISO6709", "location", "location-eng"):
        raw = tags.get(key)
        if raw:
            location = _parse_iso6709(raw)
            if location:
                break

    captured_at = None
    creation_time = tags.get("creation_time") or tags.get(
        "com.apple.quicktime.creationdate"
    )
    if creation_time:
        try:
            parsed = datetime.fromisoformat(creation_time.replace("Z", "+00:00"))
            captured_at = parsed.replace(
                tzinfo=None
            )  # normalize to naive, matching EXIF/mtime timestamps
        except ValueError:
            captured_at = None

    return location, captured_at

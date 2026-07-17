"""Render a small bordered OpenStreetMap thumbnail for a GPS coordinate, with disk caching."""

from pathlib import Path

from PIL import Image, ImageDraw
from staticmap import CircleMarker, StaticMap

USER_AGENT = "RoadTripMovie/1.0 (https://github.com/; generated map inset)"
MARKER_COLOR = "#D9534F"
BORDER_COLOR = "white"
BORDER_WIDTH = 3


def _cache_path(cache_dir: Path, lat: float, lon: float, zoom: int, width: int, height: int) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    key = f"{round(lat, 4)}_{round(lon, 4)}_{zoom}_{width}x{height}.png"
    return cache_dir / key


def render_map(
    lat: float,
    lon: float,
    cache_dir: Path,
    width: int = 240,
    height: int = 160,
    zoom: int = 14,
) -> Image.Image:
    """Return a PIL Image of an OSM map thumbnail with a pin at (lat, lon) and a white border."""
    cache_file = _cache_path(cache_dir, lat, lon, zoom, width, height)
    if cache_file.exists():
        return Image.open(cache_file).convert("RGB")

    static_map = StaticMap(width, height, headers={"User-Agent": USER_AGENT})
    static_map.add_marker(CircleMarker((lon, lat), MARKER_COLOR, 10))
    image = static_map.render(zoom=zoom, center=(lon, lat))

    draw = ImageDraw.Draw(image)
    draw.rectangle(
        [(0, 0), (width - 1, height - 1)],
        outline=BORDER_COLOR,
        width=BORDER_WIDTH,
    )

    image.save(cache_file)
    return image

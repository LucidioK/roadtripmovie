"""CLI entry point: build a road trip movie from a folder of photos/videos and a music track."""

import argparse
from pathlib import Path

from roadtripmovie import pipeline


def _parse_resolution(value: str) -> tuple[int, int]:
    try:
        w, h = value.lower().split("x")
        return int(w), int(h)
    except ValueError:
        raise argparse.ArgumentTypeError(f"Invalid resolution '{value}', expected WxH e.g. 1920x1080")


def main():
    parser = argparse.ArgumentParser(
        description="Create a road trip movie from a folder of photos/videos, "
        "a background music track, and per-item location map insets."
    )
    parser.add_argument("input_folder", type=Path, help="Folder containing photos and videos")
    parser.add_argument("music_file", type=Path, help="Background music file (mp3/wav/wma)")
    parser.add_argument("-o", "--output", type=Path, default=Path("roadtrip_movie.mp4"), help="Output video path (default: roadtrip_movie.mp4)")
    parser.add_argument("-d", "--duration", type=float, default=5.0, help="Seconds to show each photo (default: 5)")
    parser.add_argument("--max-video-duration", type=float, default=None, help="Cap on seconds played from each source video (default: play full clip)")
    parser.add_argument("--resolution", type=_parse_resolution, default=(1920, 1080), help="Output resolution as WxH (default: 1920x1080)")
    parser.add_argument("--fps", type=int, default=30, help="Output frame rate (default: 30)")
    parser.add_argument("--zoom", type=int, default=14, help="Map zoom level (default: 14)")
    parser.add_argument("--map-size", type=_parse_resolution, default=(240, 160), help="Map inset size as WxH (default: 240x160)")
    parser.add_argument("--video-audio-volume", type=float, default=0.25, help="Volume multiplier for original video audio, ducked under music (default: 0.25)")
    parser.add_argument("--music-volume", type=float, default=1.0, help="Volume multiplier for background music (default: 1.0)")
    parser.add_argument("--no-map", action="store_true", help="Disable the location map inset entirely")
    parser.add_argument("--cache-dir", type=Path, default=None, help="Directory to cache map tiles (default: .map_cache next to output)")

    args = parser.parse_args()

    pipeline.run(
        input_folder=args.input_folder,
        music_path=args.music_file,
        output_path=args.output,
        photo_duration=args.duration,
        resolution=args.resolution,
        fps=args.fps,
        map_zoom=args.zoom,
        map_size=args.map_size,
        video_audio_volume=args.video_audio_volume,
        music_volume=args.music_volume,
        max_video_duration=args.max_video_duration,
        cache_dir=args.cache_dir,
        show_map=not args.no_map,
    )


if __name__ == "__main__":
    main()

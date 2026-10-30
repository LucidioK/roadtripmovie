# RoadTripMovie

A command-line tool that turns a folder of photos/videos and a music track into a single
road-trip movie. Each media item becomes a clip — photos become still segments, videos play
back — ordered chronologically by capture time, optionally with a small OpenStreetMap location
inset, all concatenated and mixed with background music.

## Requirements

- Python 3.10+
- `ffmpeg` (bundled via `imageio_ffmpeg`; a system-installed `ffprobe` is used for video
  metadata if present, otherwise a bundled fallback is used)

Install dependencies:

```
pip install -r requirements.txt
```

## Usage

```
python main.py <input_folder> <music_file> [-o output.mp4] [options]
```

On Windows, a single-file PowerShell alternative is also available:

```powershell
.\RoadTripMovie.ps1 <input_folder> <music_file> [-Output output.mp4] [options]
```

It requires PowerShell 7+, `ffmpeg`, and `ffprobe` on `PATH`. Media metadata scans and
segment rendering run concurrently (limited by `-MaxParallelism`, defaulting to one less
than the processor count). Map thumbnails are fetched from OpenStreetMap and cached; city
labels use the repository's local city index when its Python dependencies are available.
Use `-NoMap` to disable map downloads.

- `input_folder` — folder containing photos and videos to include
- `music_file` — background music track (mp3/wav/wma/etc.)

### Options

| Flag | Default | Description |
|---|---|---|
| `-o`, `--output` | `roadtrip_movie.mp4` | Output video path |
| `-d`, `--duration` | `5` | Seconds to show each photo |
| `--max-video-duration` | (full clip) | Cap on seconds played from each source video |
| `--resolution` | `1920x1080` | Output resolution as `WxH` |
| `--fps` | `30` | Output frame rate |
| `--zoom` | `14` | Map zoom level |
| `--map-size` | `240x160` | Map inset size as `WxH` |
| `--video-audio-volume` | `0.25` | Volume multiplier for original video audio, ducked under music |
| `--music-volume` | `1.0` | Volume multiplier for background music |
| `--no-map` | off | Disable the location map inset entirely |
| `--cache-dir` | `.map_cache` next to output | Directory to cache downloaded map tiles |

### Example

```
python main.py C:/trips/iceland music.mp3 -o iceland.mp4 -d 4 --resolution 1920x1080 --zoom 12
```

Supported photo extensions: `.jpg`, `.jpeg`, `.png`, `.heic`, `.heif`
Supported video extensions: `.mp4`, `.mov`, `.avi`, `.mkv`, `.m4v`

Media is ordered by capture time read from EXIF (photos) or container metadata (videos); if
neither is available, the file's modified time is used as a fallback (see `set_file_dates.py`
below for backfilling correct timestamps beforehand).

There is no automated test suite. Verify changes by running the CLI end-to-end against a real
(or synthetic) media folder and inspecting the output video.

## Files

### Entry point

- **`main.py`** — argparse CLI that parses the options above and delegates to
  `roadtripmovie.pipeline.run`.

### `roadtripmovie/` package

- **`pipeline.py`** — end-to-end orchestration: scan the input folder, build one rendered
  segment per media item into a temporary directory, concatenate the segments, mix in the
  background music, write the final output file, and clean up.
- **`media_scanner.py`** — walks the input folder, classifies files as photo or video by
  extension, extracts capture time and GPS location (via `gps.py`), and returns all items sorted
  chronologically. Unreadable files are skipped with a warning.
- **`gps.py`** — extracts GPS coordinates and capture date from photo EXIF data (via Pillow and
  `pillow-heif` for HEIC/HEIF) and from video container metadata (via `ffprobe` if available on
  `PATH`, otherwise by shelling out to the bundled `imageio_ffmpeg` binary with `-f ffmetadata`).
- **`map_thumbnail.py`** — renders a small bordered OpenStreetMap thumbnail for a GPS coordinate
  using `staticmap`, disk-cached by rounded lat/lon/zoom/size so repeated runs don't re-fetch
  tiles.
- **`video_builder.py`** — the core moviepy logic:
  - `build_segment` — builds one canvas-fitted clip per media item, with an optional map inset
    composited on top, and always attaches an audio track (real or silent) so every segment has
    the same stream layout.
  - `write_segment` — renders a segment straight to its own file on disk.
  - `concatenate_video_files` — joins already-rendered segment files with ffmpeg's concat
    demuxer (stream copy, no re-encode), deliberately avoiding moviepy's
    `concatenate_videoclips` so segments never have to coexist as open clips/decoders.
  - `mix_background_music` — loops/trims the music track to the video length and mixes it with
    any existing segment audio.

Deliberate memory/performance choices: photos are downscaled to the output canvas size *before*
being handed to moviepy, and segments are streamed to disk one at a time rather than held open
as a list of clips, both aimed at keeping memory and open-file-handle usage bounded on a large
trip folder. See `CLAUDE.md` for a detailed note on a moviepy resource-closing gotcha this
project works around on Windows.

### Utility scripts (repo root)

- **`set_file_dates.py`** — sets a file's Created and Modified timestamps on Windows from a
  `YYYYMMDD_HHMMSS` prefix in its filename (uses the Win32 `SetFileTime` API via `ctypes`, since
  `os.utime` only sets Modified/Accessed). Useful for backfilling correct dates so
  `media_scanner.py` orders items correctly when no EXIF/container date is present.

  ```
  python set_file_dates.py 20240715_143000_photo.jpg
  ```

- **`resize_images.py`** — downscales every image in a folder whose largest dimension exceeds a
  target size, writing resized copies to an output directory. Useful for shrinking oversized
  source photos before building a movie.

  ```
  python resize_images.py <input_folder> [-o output_dir] [--target-max 1920]
  ```

### Other

- **`requirements.txt`** — Python dependencies (`moviepy`, `Pillow`, `pillow-heif`,
  `staticmap`, `requests`).
- **`.map_cache/`** — default disk cache of downloaded map tiles (git-ignored).

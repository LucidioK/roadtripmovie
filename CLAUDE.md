# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A CLI that turns a folder of photos/videos and a music track into a single road-trip movie: each
media item becomes a clip (photos become still segments, videos play back), ordered chronologically,
optionally with a small OpenStreetMap location inset, all concatenated and mixed with background
music.

## Running it

```
pip install -r requirements.txt
python main.py <input_folder> <music_file> [-o output.mp4] [options]
```

Key options (see `main.py` for the full list): `-d/--duration` (seconds per photo), `--resolution`,
`--fps`, `--zoom`/`--map-size` (map inset), `--video-audio-volume`/`--music-volume`, `--no-map`,
`--max-video-duration`, `--cache-dir` (map tile cache, default `.map_cache` next to the output).

There is no test suite. Verify changes by running the CLI end-to-end against a real (or synthetic,
see below) media folder and inspecting the output video.

## Architecture

- `main.py` — argparse CLI, delegates to `roadtripmovie.pipeline.run`.
- `roadtripmovie/media_scanner.py` — walks the input folder, classifies files as photo/video by
  extension, extracts capture time + GPS via `gps.py`, and returns items sorted chronologically.
  Falls back to file mtime if no EXIF/container date is found — this is why `set_file_dates.py`
  (repo root) exists, to backfill correct mtimes from `YYYYMMDD_HHMMSS`-prefixed filenames.
- `roadtripmovie/gps.py` — EXIF GPS/date extraction for photos (via Pillow + pillow-heif for
  HEIC), and container-metadata GPS/date extraction for videos (via `ffprobe` if present, else a
  fallback that shells out to the bundled `imageio_ffmpeg` binary with `-f ffmetadata`).
- `roadtripmovie/map_thumbnail.py` — renders a small bordered OSM map thumbnail (via `staticmap`)
  for a GPS coordinate, disk-cached by rounded lat/lon/zoom/size so repeated runs don't re-fetch tiles.
- `roadtripmovie/video_builder.py` — the core moviepy logic:
  - `build_segment`: builds one canvas-fitted clip per media item (with optional map inset composited
    on top), always with an audio track (real or silent) so every segment has the same stream layout.
  - `write_segment`: renders a segment straight to its own file on disk.
  - `concatenate_video_files`: joins already-rendered segment files with ffmpeg's concat demuxer
    (stream copy, no re-encode) — deliberately *not* moviepy's `concatenate_videoclips`, so segments
    never have to coexist as open clips/decoders.
  - `mix_background_music`: loops/trims the music track to the video length and mixes it with any
    existing segment audio.
- `roadtripmovie/pipeline.py` — orchestrates the above: scan → per-item segment → write segment to a
  `tempfile.TemporaryDirectory` → concat segments → mix music → write final output → clean up temp dir.

Deliberate memory/performance choices (see docstrings in `video_builder.py` for the reasoning):
photos are downscaled to the output canvas size *before* being handed to moviepy, and segments are
streamed to disk one at a time rather than held open as a list of clips — both aimed at not blowing
up memory/open-file-handles on a large trip folder.

## moviepy resource-closing gotcha (Windows)

`Clip.close()` is a **no-op in the base class** — it only actually closes a reader in subclasses
that override it (`VideoFileClip`, `AudioFileClip`, `CompositeVideoClip`). This bit us in
`pipeline.py`: after `video_clip.with_audio(mixed)`, `final.audio` is a `CompositeAudioClip`, whose
`close()` is the inherited no-op — so nested `AudioFileClip` readers are silently never closed.
Similarly, `AudioLoop`'s effect rebuilds the clip via `concatenate_audioclips`, which returns a
plain `AudioClip` with no `.reader` at all, so closing *that* object doesn't touch the original
`AudioFileClip(music_path)` reader either.

Net effect: ffmpeg subprocesses kept `concatenated.mp4` and the music file open even after
`final.close()`, so `tempfile.TemporaryDirectory`'s cleanup raised
`PermissionError: [WinError 32] The process cannot access the file because it is being used by
another process`.

**Rule of thumb**: never assume calling `.close()` on a composited/effected clip closes its
sources. Keep an explicit reference to every `VideoFileClip`/`AudioFileClip` you construct
*before* any `with_audio`/`with_effects`/`subclipped`/composite wrapping, and close each of those
original objects explicitly (`pipeline.run` does this for `video_clip`, `video_clip.audio`, and the
music source clip). When debugging a similar leak, a fast repro is to synthesize tiny clips with
ffmpeg (`imageio_ffmpeg.get_ffmpeg_exe()`) rather than needing real trip media.

## Windows filesystem notes

There is no POSIX "changed"/ctime on Windows — only Created, Modified, and Accessed timestamps.
Setting Created time requires the Win32 `SetFileTime` API via `ctypes` (see `set_file_dates.py`);
`os.utime` only sets Modified/Accessed.

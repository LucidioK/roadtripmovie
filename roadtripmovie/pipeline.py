"""End-to-end orchestration: scan media, build segments, concatenate, mix music, export."""

from __future__ import annotations
from datetime import datetime

import queue
import sys
import tempfile
import threading
from pathlib import Path

from moviepy import VideoFileClip

from roadtripmovie.media_scanner import MediaItem
from . import video_builder
from .media_scanner import scan_media

# ANSI color codes for terminal output
GREEN = "\033[92m"
CYAN = "\033[96m"
YELLOW = "\033[0;33m"
RED = "\033[0;31m"
NO_COLOR = "\033[0m"


class Pipeline:
    """Orchestrate the entire road trip movie creation process."""

    def __init__(
        self,
        input_folder: Path,
        music_path: Path,
        output_path: Path,
        font: str,
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
        if not Path(music_path).is_file() or Path(music_path).suffix.lower() not in [
            ".mp3",
            ".wav",
            ".aac",
            ".flac",
        ]:
            raise ValueError(f"Invalid music file: {music_path}")
        if not Path(input_folder).is_dir():
            raise ValueError(f"Input folder does not exist: {input_folder}")
        self.input_folder: Path = input_folder
        self.music_path: Path = music_path
        self.output_path: Path = output_path
        self.font: str = font
        self.photo_duration: float = photo_duration
        self.resolution: tuple[int, int] = resolution
        self.fps: int = fps
        self.map_zoom: int = map_zoom
        self.map_size: tuple[int, int] = map_size
        self.video_audio_volume: float = video_audio_volume
        self.music_volume: float = music_volume
        self.max_video_duration: float | None = max_video_duration
        self.cache_dir: Path | None = cache_dir
        self.show_map: bool = show_map
        self.enumerated_items: list[tuple[int, any]] = []
        self.segment_files: queue.Queue[Path] = queue.Queue()
        self.lock: threading.Lock = threading.Lock()
        self.tmp_path: Path | None = None
        self.start_time: datetime = datetime.now()
        print(
            f"{GREEN}Python version: {YELLOW}"
            f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}{GREEN}, "
            f"GIL enabled: {YELLOW}{sys._is_gil_enabled()}{NO_COLOR}"
        )
        self.central_loop: callable = self._sequential_loop
        if sys.version_info >= (3, 14):
            if not sys._is_gil_enabled():
                self.central_loop = self._multi_threaded_loop

    def _build_segment(self, index: int, item: MediaItem) -> None:
        print(
            f"{GREEN}Processing {YELLOW}{index}: {item.path.name} {GREEN}({CYAN}{item.kind}"
            f"{GREEN}, {CYAN}{item.captured_at}{GREEN})...{NO_COLOR}"
        )
        try:
            print(
                f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}{GREEN}: build...{NO_COLOR}"
            )
            if self.segment_files.qsize() > 1:
                time_until_now = datetime.now() - self.start_time
                built_segment_count = self.segment_files.qsize()
                predicted_total_time = (time_until_now / built_segment_count) * (
                    len(self.enumerated_items) - built_segment_count
                )
                print(
                    f"{GREEN}  Took {YELLOW}{time_until_now}{GREEN} until now,  "
                    f"for {YELLOW}{built_segment_count}{GREEN} segments, "
                    f"for {YELLOW}{len(self.enumerated_items)-built_segment_count}{GREEN} more segments, "
                    f"{YELLOW}{predicted_total_time}{GREEN} predicted total time{NO_COLOR}"
                )
            segment = video_builder.build_segment(
                item=item,
                photo_duration=self.photo_duration,
                target_w=self.resolution[0],
                target_h=self.resolution[1],
                map_cache_dir=self.cache_dir,
                video_audio_volume=self.video_audio_volume,
                map_zoom=self.map_zoom,
                map_width=self.map_size[0],
                map_height=self.map_size[1],
                max_video_duration=self.max_video_duration,
                show_map=self.show_map,
                font=self.font,
            )
        except Exception as exc:
            print(
                f"{RED}Warning: skipping unreadable file {item.path}: {exc} \n {exc.__traceback__}"
                f"{NO_COLOR}",
                file=sys.stderr,
            )
            return None

        segment_file = self.tmp_path / f"segment_{index:05d}.mp4"
        print(
            f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}{GREEN}: writing to {YELLOW}"
            f"{segment_file}{GREEN}...{NO_COLOR}"
        )
        video_builder.write_segment(segment, segment_file, fps=self.fps)
        print(
            f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}"
            f"{GREEN}: appending to segment files...{NO_COLOR}"
        )
        self.segment_files.put(segment_file)

    def _sequential_loop(self) -> None:
        start = datetime.now()
        print(f"{GREEN}Processing media items {YELLOW}sequentially{GREEN}...{NO_COLOR}")
        for index, item in self.enumerated_items:
            self._build_segment(index, item)
        end = datetime.now()
        print(
            f"{GREEN}Finished processing media items {YELLOW}sequentially{GREEN} in {CYAN}"
            f"{(end - start)}{NO_COLOR}"
        )

    def _multi_threaded_loop(self) -> None:
        start = datetime.now()
        print(f"{GREEN}Processing media items {YELLOW}in parallel{GREEN}...{NO_COLOR}")
        from concurrent.futures import ThreadPoolExecutor

        with ThreadPoolExecutor(max_workers=3) as executor:
            executor.map(lambda args: self._build_segment(*args), self.enumerated_items)
        end = datetime.now()
        print(
            f"{GREEN}Finished processing media items {YELLOW}in parallel{GREEN} in {CYAN}"
            f"{(end - start)}{NO_COLOR}"
        )

    def run(self):
        """
        Run the pipeline.
        This method orchestrates the entire process of scanning media, building segments,
        concatenating them, mixing background music, and exporting the final video.
        """
        items = scan_media(self.input_folder)
        print(
            f"{GREEN}Found {YELLOW}{len(items)}{GREEN} media items in {YELLOW}{self.input_folder}"
            f"{GREEN}{NO_COLOR}"
        )
        if not items:
            raise SystemExit(f"No supported photos/videos found in {self.input_folder}")

        if self.cache_dir is None:
            self.cache_dir = self.output_path.parent / ".map_cache"

        self.enumerated_items = list(enumerate(items))

        with tempfile.TemporaryDirectory(prefix="roadtripmovie_") as tmp_dir:
            self.tmp_path = Path(tmp_dir)
            self.start_time = datetime.now()
            print(f"{GREEN}Temporary directory: {YELLOW}{self.tmp_path}{NO_COLOR}")
            self.central_loop()  # Process media items to build segments

            if not self.segment_files:
                raise SystemExit("No media files could be processed successfully.")

            print(f"{GREEN}Concatenating segments...{NO_COLOR}")
            concatenated_path = self.tmp_path / "concatenated.mp4"
            sorted_segment_files = sorted(
                list(self.segment_files.queue), key=lambda p: p.name
            )  # Sort segment files by name
            video_builder.concatenate_video_files(
                sorted_segment_files, concatenated_path
            )

            print(f"{GREEN}Mixing background music...{NO_COLOR}")
            video_clip = VideoFileClip(str(concatenated_path))
            final, music_clip = video_builder.mix_background_music(
                video_clip, str(self.music_path), self.music_volume
            )

            try:
                print(f"{GREEN}Writing {YELLOW}{self.output_path}{GREEN}...{NO_COLOR}")
                final.write_videofile(
                    str(self.output_path),
                    fps=self.fps,
                    codec="libx264",
                    audio_codec="aac",
                )
            finally:
                final.close()
                if video_clip.audio is not None:
                    video_clip.audio.close()
                video_clip.close()
                music_clip.close()


def run(
    input_folder: Path,
    music_path: Path,
    output_path: Path,
    font: str,
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
    """
    Run the road trip movie pipeline.
    """
    pipeline = Pipeline(
        input_folder=input_folder,
        music_path=music_path,
        output_path=output_path,
        photo_duration=photo_duration,
        resolution=resolution,
        fps=fps,
        map_zoom=map_zoom,
        map_size=map_size,
        video_audio_volume=video_audio_volume,
        music_volume=music_volume,
        max_video_duration=max_video_duration,
        cache_dir=cache_dir,
        show_map=show_map,
        font=font,
    )
    pipeline.run()

"""End-to-end orchestration: scan media, build segments, concatenate, mix music, export."""

from __future__ import annotations
from datetime import datetime
import os
import queue
import sys
import tempfile
import time
import traceback
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
        # max_parallelism: int | None = None,
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
        # self.max_parallelism: int | None = max_parallelism or 1
        self.enumerated_items: list[tuple[int, any]] = []
        self.segment_files: queue.Queue[Path] = queue.Queue()
        # self.lock: multiprocessing.Lock = multiprocessing.Lock()
        self.tmp_path: Path | None = None
        self.start_time: datetime = datetime.now()
        print(
            f"{GREEN}Python version: {YELLOW}"
            f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}{GREEN}, "
            f"GIL enabled: {YELLOW}{sys._is_gil_enabled()}{NO_COLOR}"
        )
        self.central_loop: callable = self._sequential_loop
        # if self.max_parallelism > 1 and sys.version_info >= (3, 14) and not sys._is_gil_enabled():
        #     self.central_loop = self._multi_threaded_loop
        # print(f"{GREEN}Pipeline instance initialized: {YELLOW}"
        #       f"{json.dumps(self.__dict__, indent=2, default=str)}{NO_COLOR}")

    def build_segment(self, item: MediaItem) -> None:
        """
        Build a video segment for the given media item.

        Args:
            item (MediaItem): The media item to build the segment for.

        Returns:
            None, but the segment is added to the internal queue for later processing.
        """
        index = item.index
        try:
            print(
                f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}{GREEN}: build...{NO_COLOR}"
            )
            if self.segment_files.qsize() > 1:
                time_until_now = datetime.now() - self.start_time
                built_segment_count = self.segment_files.qsize()
                still_to_build = len(self.enumerated_items) - built_segment_count
                predicted_total_time = (time_until_now / built_segment_count) * still_to_build
                print(
                    f"{GREEN}  Took {YELLOW}{time_until_now}{GREEN} until now  "
                    f"for {YELLOW}{built_segment_count}{GREEN} segments, "
                    f"for {YELLOW}{still_to_build}{GREEN} more segments, "
                    f"{YELLOW}{predicted_total_time}{GREEN} more time{NO_COLOR}"
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
        except (OSError, ValueError, RuntimeError) as exc:
            print(
                f"{RED}Warning: build_segment skipping unreadable file {item.path}: {exc} \n"
                f"{exc.__traceback__}{NO_COLOR}",
                file=sys.stderr,
            )
            traceback.print_exc()
            return None

        segment_file = self.tmp_path / f"segment_{index:05d}.mp4"
        print(
            f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}{GREEN}: writing to {YELLOW}"
            f"{segment_file}{GREEN}...{NO_COLOR}"
        )
        for attempt in range(3):
            try:
                video_builder.write_segment(segment, segment_file, fps=self.fps)
                break
            except BrokenPipeError as exc:
                print(
                    f"{RED}Warning: write_segment failed with BrokenPipeError for {item.path.name}"
                    f" (attempt {attempt + 1}): {exc} \n {exc.__traceback__}{NO_COLOR}",
                    file=sys.stderr,
                )
                traceback.print_exc()
                if attempt == 2:
                    return None
                else:
                    time.sleep(5)
        print(
            f"{GREEN}  Processing {YELLOW}{index}: {item.path.name}"
            f"{GREEN}: appending to segment files...{NO_COLOR}"
        )
        self.segment_files.put(segment_file)

    def _sequential_loop(self) -> None:
        start = datetime.now()
        print(f"{GREEN}Processing media items {YELLOW}sequentially{GREEN}...{NO_COLOR}")
        if Path("pipeline_state.json").exists():
            os.unlink("pipeline_state.json")
        for _, item in self.enumerated_items:
            self.build_segment(item)
        end = datetime.now()
        print(
            f"{GREEN}Finished processing media items {YELLOW}sequentially{GREEN} in {CYAN}"
            f"{(end - start)}{NO_COLOR}"
        )

    # This does not work...
    # def _multi_threaded_loop(self) -> None:
    #     from concurrent.futures import ThreadPoolExecutor
    #     start = datetime.now()
    #     max_workers = self.max_parallelism or 2
    #     print(f"{GREEN}Processing media items {YELLOW}in parallel{GREEN} with "
    #           f"{YELLOW}{max_workers}{GREEN} workers...  {self.__hash__()}{NO_COLOR}")
    #     with ThreadPoolExecutor(max_workers=max_workers) as executor:
    #         tasks = list(executor.map(self.build_segment, [
    #                      item[1] for item in self.enumerated_items]))

    #     end = datetime.now()
    #     print(
    #         f"{GREEN}Finished processing media items {YELLOW}in parallel{GREEN} in {CYAN}"
    #         f"{(end - start)}{NO_COLOR}"
    #     )

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

            self.segment_files = queue.Queue()
            for file in sorted(self.tmp_path.iterdir()):
                self.segment_files.put(file)
            if self.segment_files.empty():
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
        # max_parallelism=max_parallelism,
    )
    pipeline.run()

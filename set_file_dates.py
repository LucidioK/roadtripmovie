#!/usr/bin/env python3
"""Set a file's Created and Modified dates from a YYYYMMDD_HHMMSS timestamp in its name."""

from __future__ import annotations

import argparse
import ctypes
import os
import re
from datetime import datetime
from pathlib import Path

TIMESTAMP_RE = re.compile(r"^(\d{8})_(\d{6})")

# Windows FILETIME: 100-ns intervals since 1601-01-01, seconds offset from the Unix epoch.
FILETIME_EPOCH_OFFSET = 116444736000000000
FILETIME_TICKS_PER_SECOND = 10_000_000

GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_FLAG_BACKUP_SEMANTICS = 0x02000000


class _FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", ctypes.c_uint32), ("dwHighDateTime", ctypes.c_uint32)]


def parse_timestamp(file_name: str) -> datetime:
    match = TIMESTAMP_RE.match(file_name)
    if not match:
        raise ValueError(f"File name does not start with a YYYYMMDD_HHMMSS timestamp: {file_name}")
    return datetime.strptime(f"{match.group(1)}_{match.group(2)}", "%Y%m%d_%H%M%S")


def _to_filetime(dt: datetime) -> _FILETIME:
    ticks = int(dt.timestamp() * FILETIME_TICKS_PER_SECOND) + FILETIME_EPOCH_OFFSET
    return _FILETIME(ticks & 0xFFFFFFFF, ticks >> 32)


def set_created_time(path: Path, dt: datetime) -> None:
    handle = ctypes.windll.kernel32.CreateFileW(
        str(path),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        None,
    )
    if handle == -1:
        raise ctypes.WinError(ctypes.get_last_error())

    try:
        creation_time = _to_filetime(dt)
        if not ctypes.windll.kernel32.SetFileTime(handle, ctypes.byref(creation_time), None, None):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def set_file_dates(path: Path, dt: datetime) -> None:
    set_created_time(path, dt)
    timestamp = dt.timestamp()
    os.utime(path, (timestamp, timestamp))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Set a file's Created and Modified dates from a YYYYMMDD_HHMMSS prefix in its file name."
    )
    parser.add_argument("file", type=Path, help="Path to the file whose name starts with YYYYMMDD_HHMMSS")
    args = parser.parse_args()

    file_path = args.file.resolve()
    if not file_path.is_file():
        raise SystemExit(f"File does not exist: {file_path}")

    dt = parse_timestamp(file_path.name)
    set_file_dates(file_path, dt)
    print(f"Set Created and Modified dates of {file_path.name} to {dt.isoformat(sep=' ')}")


if __name__ == "__main__":
    main()

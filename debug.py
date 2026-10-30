import subprocess
import inspect
import json
import os
import platform
import shutil
import sys
from datetime import datetime, timezone, timedelta
this_folder = os.path.dirname(os.path.abspath(inspect.getfile(inspect.currentframe())))
import re

current_os = platform.system()
if current_os == "Windows":
    subprocess.call(
        ["python", f"{this_folder}/main.py", 
        "c:/Temp/FranceFew", "c:/Temp/FranceTrip.mp3", "-o", "c:/Temp/DeleteMe2.mp4", "--zoom", "7", "--map-size", "480x320", "--max-parallelism", "4"]
    )
elif current_os == "Linux":
    subprocess.call(
        ["python", f"{this_folder}/main.py", 
        "/mnt/c/Temp/FranceFew", "/mnt/c/Temp/FranceTrip.mp3", "-o", "DeleteMe2.mp4", "--zoom", "7", "--map-size", "480x320", "--max-parallelism", "4"]
    )
else:
    print(f"Unknown operating system: {current_os}")

exit(0)


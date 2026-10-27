import subprocess
import inspect
import json
import os
import shutil
import sys
from datetime import datetime, timezone, timedelta
this_folder = os.path.dirname(os.path.abspath(inspect.getfile(inspect.currentframe())))
import re

subprocess.call(
    ["python", f"{this_folder}/main.py", 
     "c:/Temp/FranceFew", "c:/Temp/FranceTrip.mp3", "-o", "c:/Temp/DeleteMe2.mp4", "--zoom", "7", "--map-size", "480x320"]
)
exit(0)

subprocess.call(
    ["python", f"{this_folder}/main.py", 
     "/mnt/c/Temp/FranceFew", "/mnt/c/Temp/FranceTrip.mp3", "-o", "DeleteMe2.mp4", "--zoom", "7", "--map-size", "480x320"]
)
exit(0)


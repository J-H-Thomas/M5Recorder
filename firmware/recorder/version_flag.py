"""PlatformIO build flag with the firmware version: the git short hash, plus
"-dirty" if there are uncommitted changes. Used as build_flags = !python version_flag.py"""

import subprocess

try:
    version = subprocess.run(["git", "describe", "--always", "--dirty"],
                             capture_output=True, text=True, check=True).stdout.strip()
except Exception:
    version = "unknown"
print(f'-DFW_VERSION=\\"{version}\\"')

import os
import re
import subprocess

Import("env")

version = os.environ.get("FIRMWARE_VERSION", "").strip()
if not version:
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        version = "unknown"

version = re.sub(r"[^A-Za-z0-9._+-]", "-", version) or "unknown"
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", f'\\"{version}\\"')])

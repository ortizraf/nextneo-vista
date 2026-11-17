# PlatformIO pre-script:
#  1. names the firmware after the app (nextneo-vista.bin / .elf) instead of firmware.bin;
#  2. writes src/nextneo-vista/core/build_info.cpp (gitignored) with the version
#     (`git describe --tags --always --dirty`) and the build time (Unix epoch, UTC).
#     Only that small file is recompiled on each build. Shown on the web settings page.
import subprocess
import time
from pathlib import Path

Import("env")
env.Replace(PROGNAME="nextneo-vista")

root = Path(env.subst("$PROJECT_DIR"))


def git(*args):
    try:
        # safe.directory: inside Docker the repo belongs to another uid
        return subprocess.check_output(["git", "-c", "safe.directory=*", "-C", str(root), *args],
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


version = git("describe", "--tags", "--always", "--dirty") or "sem-git"
branch = git("rev-parse", "--abbrev-ref", "HEAD")
epoch = int(time.time())

src = f"""// GERADO por tools/pio_progname.py a cada build. Não edite; não vai para o git.
#include "core/build_info.h"
const char *const kBuildVersion = "{version}";
const char *const kBuildBranch = "{branch}";
const unsigned long kBuildEpoch = {epoch}UL;
"""
out = root / "src" / "nextneo-vista" / "core" / "build_info.cpp"
if not out.exists() or out.read_text() != src:
    out.write_text(src)
print(f"Build info: {version} ({branch}) epoch {epoch}")

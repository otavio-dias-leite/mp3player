Import("env")
from pathlib import Path
import re

version_path = Path("VERSION")
version = version_path.read_text(encoding="utf-8").strip()
if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?", version):
    raise RuntimeError("VERSION must contain a valid SemVer value")

header = Path("include/mp3_player_version.h")
header.parent.mkdir(parents=True, exist_ok=True)
header.write_text(
    "#pragma once\n"
    "#define MP3_PLAYER_VERSION \"" + version + "\"\n",
    encoding="utf-8",
)
print("MP3 Player firmware version: " + version)
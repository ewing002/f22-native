"""Makes the release zip: the programs and the notes only, never game data.

usage: py tools/package.py <build folder holding f22native.exe and f22launcher.exe> [out.zip, default: F22Native-release.zip in the build folder]
"""
import os
import sys
import zipfile

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(build, "F22Native-release.zip")
exes = ["f22native.exe", "f22launcher.exe"]
for e in exes:
    if not os.path.exists(os.path.join(build, e)):
        sys.exit("missing " + e + " in " + build)

INSTALL = r"""F-22 Lightning II - native rewrite
=================================

You need your own copy of the game; this zip contains no game data.

1. Copy f22native.exe and f22launcher.exe into your F-22 game folder (the one that holds F22.EXE and RESOURCE.RES).
   F22.EXE is decrypted in memory at every start; no extra file has to be made and the game files are not changed.
2. Start f22launcher.exe and press Play (or start f22native.exe directly).
   If the game folder is elsewhere, choose it in the launcher with "Browse".
3. Optional videos: py tools\f22videos.py "<CD>\RESOURCE.RES" "<game folder>\video"
4. Optional replaceable textures: "Export textures" in the launcher, then edit the PNGs in the textures folder.
Keys, options and details: README.md and docs\FEATURES.md.
"""

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for e in exes:
        z.write(os.path.join(build, e), "F22Native/" + e)
    z.writestr("F22Native/INSTALL.txt", INSTALL)
    for f in ("README.md",):
        z.write(os.path.join(here, f), "F22Native/" + f)
    for base in ("docs", "tools"):
        for b, _, names in os.walk(os.path.join(here, base)):
            for n in names:
                if n.endswith(".pyc"):
                    continue
                p = os.path.join(b, n)
                z.write(p, "F22Native/" + os.path.relpath(p, here).replace(os.sep, "/"))
print("packaged", out)

"""Makes the release zip: the program and the notes only, never game data.

usage: py tools/package.py <build folder holding f22native.exe> [out.zip, default: F22Native-release.zip in the build folder]
"""
import os
import sys
import zipfile

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(build, "F22Native-release.zip")
exe = os.path.join(build, "f22native.exe")
if not os.path.exists(exe):
    sys.exit("missing f22native.exe in " + build)

INSTALL = r"""F-22 Lightning II - native rewrite
=================================

You need your own copy of the game; this zip contains no game data.

1. Put f22native.exe into your F-22 game folder (the one that holds RESOURCE.RES), or into a subfolder of it.
2. Make F22_decrypted.bin from your F22.EXE (once), next to RESOURCE.RES:
     py -m pip install unicorn
     py tools\f22unpack.py --exe "<game folder>\F22.EXE" -o "<game folder>\F22_decrypted.bin"
   (the tools folder is in this zip and in the source repository.)
3. Optional videos: py tools\f22videos.py "<CD>\RESOURCE.RES" "<game folder>\video"
4. Optional replaceable textures: run textura_kiiras.bat once, then edit the PNGs in the textures folder.
5. Start f22native.exe. Keys, options and details: docs\FEATURES.md and README.md.
"""

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(exe, "F22Native/f22native.exe")
    z.writestr("F22Native/INSTALL.txt", INSTALL)
    z.writestr("F22Native/textura_kiiras.bat", "@echo off\r\nrem Dumps the original textures as PNG into the textures folder (editable, replaceable by name).\r\ncd /d \"%~dp0\"\r\nf22native.exe --dumptex\r\necho Done: see the textures folder.\r\npause\r\n")
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

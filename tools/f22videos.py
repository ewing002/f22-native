#!/usr/bin/env python3
r"""Extracts the KDV videos from the CD's RESOURCE.RES (the ~409 MB one) into a flat folder for f22native.exe.

usage: py tools/f22videos.py "<CD>\RESOURCE.RES" "<game folder>\video"
The program looks for the videos in a 'video' folder next to the exe (HAIR.KDV, HF22INTR.KDV, ...). Read-only on the source.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f22res import read_index  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    res, out = sys.argv[1], sys.argv[2]
    with open(res, "rb") as f:
        data = f.read()
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, start, size in read_index(data):
        if name.upper().endswith(".KDV"):
            with open(os.path.join(out, name), "wb") as f:
                f.write(data[start:start + size])
            print(f"{name:16} {size:10} bytes")
            n += 1
    print(f"{n} videos written to {out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""F-22 Lightning II (NovaLogic, 1996) RESOURCE.RES extractor.

Format (reverse-engineered):
  0x00  12 bytes   "RESOURCE2xxx"
  0x0C  u32       entry count (N)
  0x10  N * 20    index: name[12] (XOR with key 0xACEDDEAD, repeating every 4 bytes),
                         u32 offset, u32 size
  Each file's data starts at offset+20. The 20 bytes at the offset are
  a copy of the NEXT index entry (a linked list for sequential reading).
  The first entry ("ROOT", size 0) is only the start of the chain.
  The data is not encrypted.

Usage:
  py tools/f22res.py list   [RESOURCE.RES]
  py tools/f22res.py extract [RESOURCE.RES] [-o output] [--png] [pattern ...]
"""
import argparse
import fnmatch
import os
import struct
import sys
import zlib

MAGIC = b"RESOURCE2xxx"
KEY = bytes([0xAD, 0xDE, 0xED, 0xAC])
ENTRY = 20
HEADER = 20


def xor_name(raw):
    return bytes(b ^ KEY[i % 4] for i, b in enumerate(raw))


def read_index(data):
    if data[:12] != MAGIC:
        raise ValueError("not a RESOURCE2 archive")
    (count,) = struct.unpack_from("<I", data, 12)
    entries = []
    for i in range(count):
        pos = 16 + i * ENTRY
        raw = data[pos:pos + 12]
        off, size = struct.unpack_from("<II", data, pos + 12)
        name = xor_name(raw).rstrip(b"\0")
        if size == 0:  # ROOT / chain-start entry
            continue
        name = name.decode("ascii")
        start = off + HEADER
        if start + size > len(data):
            raise ValueError(f"{name}: extends past the end of the file")
        entries.append((name, start, size))
    return entries


# ---- PCX -> PNG (8-bit, single-plane, RLE PCX only; this is what the game uses) ----

def decode_pcx(buf):
    manuf, ver, enc, bpp = buf[0], buf[1], buf[2], buf[3]
    if manuf != 0x0A or bpp != 8 or buf[65] != 1:
        raise ValueError("unsupported PCX")
    xmin, ymin, xmax, ymax = struct.unpack_from("<HHHH", buf, 4)
    bpl = struct.unpack_from("<H", buf, 66)[0]
    w, h = xmax - xmin + 1, ymax - ymin + 1
    out = bytearray()
    need = bpl * h
    i = 128
    end = len(buf) - 769 if len(buf) >= 769 and buf[-769] == 0x0C else len(buf)
    while len(out) < need and i < end:
        b = buf[i]
        i += 1
        if enc == 1 and b >= 0xC0:
            out += bytes([buf[i]]) * (b & 0x3F)
            i += 1
        else:
            out.append(b)
    out = out[:need].ljust(need, b"\0")
    pixels = b"".join(out[y * bpl:y * bpl + w] for y in range(h))
    if end != len(buf):
        pal = bytes(buf[end + 1:end + 769])
    else:  # no palette: grayscale
        pal = bytes(c for v in range(256) for c in (v, v, v))
    return w, h, pixels, pal


def write_png(path, w, h, pixels, pal, trns=None):
    """8-bit paletted PNG; trns: per-palette-index alpha (optional)."""
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + pixels[y * w:(y + 1) * w] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)))
        f.write(chunk(b"PLTE", pal))
        if trns is not None:
            f.write(chunk(b"tRNS", bytes(trns)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_res = os.path.join(os.path.dirname(here), "RESOURCE.RES")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["list", "extract"])
    ap.add_argument("res", nargs="?", default=default_res)
    ap.add_argument("patterns", nargs="*", help="filename patterns, e.g. *.PCX")
    ap.add_argument("-o", "--out", default=None, help="kimeneti mappa")
    ap.add_argument("--png", action="store_true", help="also convert PCX images to PNG")
    a = ap.parse_args()
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8")

    # if the first "pattern" is actually a nonexistent file, treat it as a pattern
    if not os.path.isfile(a.res):
        a.patterns.insert(0, a.res)
        a.res = default_res

    with open(a.res, "rb") as f:
        data = f.read()
    entries = read_index(data)
    if a.patterns:
        entries = [e for e in entries
                   if any(fnmatch.fnmatch(e[0].upper(), p.upper()) for p in a.patterns)]

    if a.cmd == "list":
        for name, off, size in entries:
            print(f"{name:14} {off:10} {size:10}")
        print(f"{len(entries)} files, {sum(e[2] for e in entries)} bytes")
        return

    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.res)), "extracted")
    os.makedirs(out, exist_ok=True)
    png_dir = os.path.join(out, "png")
    pngs = fails = 0
    for name, off, size in entries:
        blob = data[off:off + size]
        ext = os.path.splitext(name)[1].lstrip(".").upper() or "NOEXT"
        d = os.path.join(out, ext)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(blob)
        if a.png and ext == "PCX":
            try:
                os.makedirs(png_dir, exist_ok=True)
                write_png(os.path.join(png_dir, name[:-4] + ".png"), *decode_pcx(blob))
                pngs += 1
            except Exception as e:  # noqa: BLE001
                fails += 1
                print(f"PNG hiba: {name}: {e}", file=sys.stderr)
    print(f"{len(entries)} files extracted to: {out}")
    if a.png:
        print(f"{pngs} PNG created, {fails} errors")


if __name__ == "__main__":
    main()

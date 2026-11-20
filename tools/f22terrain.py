#!/usr/bin/env python3
"""F-22 Lightning II terrain exporter (reads directly from RESOURCE.RES).

Reverse-engineered formats
------------------------
BINxx.BIN  (4 files, the tile set of one theater each; 2,102,796 bytes)
  0x000  u32        tile count = 128
  0x004  128 * u32  tile offsets: 0x07000000 | (k * 0x4000)
                    (the upper byte 7 = log2(128), the exponent of the tile side length;
                     the lower 24 bits are the offset relative to the data start at 0x208)
  0x204  u32        data size = 0x200000
  0x208  128 * 16384 bytes: 128x128-pixel, 8-bit paletted tiles, row by row
  +0     u32        1024 (palette size in bytes)
  +4     256 * 4    palette, Windows RGBQUAD order: B, G, R, 0 (8-bit)
  +1028  16 * 256   shading/fog table: 16 light levels x 256 colors -> palette index

xxxx.REF   (per-mission map, 24,576 bytes)
  0x0000  128 * 128 bytes  tile index (0..127) per cell, row by row
  0x4000  128 * 128 * 4 bit  orientation code per cell; 2 cells per byte,
                             the low nibble is the first (even) cell
  The full map is 128 x 128 cells * 128 px = 16384 x 16384 pixels.

Orientation codes (measured from how the tile borders match; code 7 is the rarest,
so it is less certain):
  0 original   1 horizontal flip   2 vertical flip   3 180° rotation
  4 90° rotation (transpose + horizontal flip)
  5 90° rotation the other way (transpose + vertical flip)
  6 transpose + both flips (flip along the anti-diagonal)
  7 transpose (flip along the main diagonal)

Which BIN belongs to a map: the low nibble of the first byte of its
.ORF (0..3 -> BIN01..BIN04); for the MREGxx maps this is not filled in, so
the tool checks it based on how the tile borders match and, if necessary,
overrides it.

DEPTHnn.PCX / SHADOWnn.PCX  (per theater, the pair of BINnn)
  512 x 512, 8-bit gray PCX. DEPTH is the height map (0 = sea level),
  SHADOW is the precomputed shadow/lighting. They cover the whole area
  of the map: 1 sample = 4 x 4 cells / 16, i.e. 32 x 32 texture pixels.
  Source: the loader at 0x511AC in the reverse-engineered F22.EXE loads
  bin0n.bin, depth0n.pcx and shadow0n.pcx with the same theater index.
  The terrain of theaters 01 and 02 is almost identical (the shadow map
  matches byte for byte). All four DEPTH maps fit without rotation/offset
  to the tile map. In theater 04 (jungle) the tile type hardly reveals
  the height (forest tiles occur at every height), so
  it seemed not to fit; the water (tile 8), however, has exactly 0 height in 99.6% of its cells,
  and even a 1-cell offset worsens the match. Here
  0 is not only sea: flat lowland also has height 0.

Usage:
  py tools/f22terrain.py                     # every unique map, 1/8 size
  py tools/f22terrain.py QCKMIS1 --scale 1   # full 16384x16384 resolution
  py tools/f22terrain.py --list
"""
import argparse
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f22res import decode_pcx, read_index, write_png  # noqa: E402

TILE = 128
NT = 128
GRID = 128
BIN_DATA = 0x208
TRAILER = BIN_DATA + NT * TILE * TILE

# orientation code -> (transpose, horizontal flip, vertical flip);
# the transpose happens first
ORIENT = [
    (0, 0, 0), (0, 1, 0), (0, 0, 1), (0, 1, 1),
    (1, 1, 0), (1, 0, 1), (1, 1, 1), (1, 0, 0),
]


class Res:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.idx = {n.upper(): (o, s) for n, o, s in read_index(self.data)}

    def get(self, name):
        o, s = self.idx[name.upper()]
        return self.data[o:o + s]


class Tileset:
    def __init__(self, blob):
        n = struct.unpack_from("<I", blob, 0)[0]
        assert n == NT, n
        offs = struct.unpack_from("<%dI" % n, blob, 4)
        self.tiles = [blob[BIN_DATA + (o & 0xFFFFFF):BIN_DATA + (o & 0xFFFFFF) + TILE * TILE]
                      for o in offs]
        t = blob[TRAILER:]
        assert struct.unpack_from("<I", t, 0)[0] == 1024
        self.pal = bytes(t[4 + i * 4 + 2 - c] for i in range(256) for c in range(3))
        self.shade = t[1028:1028 + 4096]
        self._lum = [(self.pal[i * 3] * 3 + self.pal[i * 3 + 1] * 6 + self.pal[i * 3 + 2]) / 10
                     for i in range(256)]
        self._cache = {}

    def oriented(self, k, code, step=1):
        key = (k, code, step)
        if key not in self._cache:
            n = TILE // step
            off = step // 2
            src = self.tiles[k]
            g = [bytes(src[(off + y * step) * TILE + off + x * step] for x in range(n))
                 if step > 1 else src[y * TILE:(y + 1) * TILE] for y in range(n)]
            tr, fx, fy = ORIENT[code]
            if tr:
                g = [bytes(g[x][y] for x in range(n)) for y in range(n)]
            if fx:
                g = [r[::-1] for r in g]
            if fy:
                g = g[::-1]
            self._cache[key] = b"".join(g)
        return self._cache[key]


def parse_ref(blob):
    idx = blob[:GRID * GRID]
    codes = bytes(v for b in blob[GRID * GRID:GRID * GRID * 3 // 2] for v in (b & 15, b >> 4))
    return idx, codes


def seam_score(ts, idx, codes, stride=6):
    L = ts._lum
    tot = n = 0
    for cy in range(0, GRID - 1, stride):
        for cx in range(0, GRID - 1, stride):
            i = cy * GRID + cx
            a = ts.oriented(idx[i], codes[i])
            r = ts.oriented(idx[i + 1], codes[i + 1])
            d = ts.oriented(idx[i + GRID], codes[i + GRID])
            for k in range(0, TILE, 8):
                tot += abs(L[a[k * TILE + TILE - 1]] - L[r[k * TILE]])
                tot += abs(L[a[(TILE - 1) * TILE + k]] - L[d[k]])
                n += 2
    return tot / n


def parse_orf(blob):
    """Mission object list (.ORF).

    0x284  u32 n, 0x288  n * 16 bytes model table (file name, e.g. "HNG_03.PAK")
    then   u32 count, then 40 bytes per record:
      +0 x, +4 y (height; -2 = on the ground), +8 z   (i32, world units)
      +12 u8 category, +13 u8 side, +14 u16 heading (65536 = 360°), +16 u16 model row
    World unit: 1 map cell = 512, 1 texture pixel = 4; the model coordinates are
    the 16.16 fixed-point forms of the same. After the records come waypoints and texts
    (not processed).
    """
    n = struct.unpack_from("<I", blob, 0x284)[0]
    slots = [cstr16(blob[0x288 + i * 16:0x298 + i * 16]) for i in range(n)]
    base = 0x288 + n * 16
    cnt = struct.unpack_from("<I", blob, base)[0]
    objs = []
    for k in range(cnt):
        p = base + 4 + k * 40
        if p + 40 > len(blob):
            break
        x, y, z = struct.unpack_from("<3i", blob, p)
        cat, side, head, slot = struct.unpack_from("<BBHH", blob, p + 12)
        model = slots[slot].upper().removesuffix(".PAK") if slot < n else ""
        if model:
            objs.append({"m": model, "x": x, "y": y, "z": z, "h": head, "cat": cat, "side": side})
    return objs


def cstr16(b):
    return b.split(b"\0")[0].decode("ascii", "replace")


def compose(ts, idx, codes, scale):
    n = TILE // scale
    W = GRID * n
    rows = bytearray(W * W)
    for cy in range(GRID):
        for cx in range(GRID):
            i = cy * GRID + cx
            t = ts.oriented(idx[i], codes[i], scale)
            for y in range(n):
                p = (cy * n + y) * W + cx * n
                rows[p:p + n] = t[y * n:(y + 1) * n]
    return W, bytes(rows)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="*", help="map names, e.g. QCKMIS1 C01M01")
    ap.add_argument("--res", default=os.path.join(root, "RESOURCE.RES"))
    ap.add_argument("-o", "--out", default=os.path.join(root, "extracted", "terrain"))
    ap.add_argument("--scale", type=int, default=8, choices=[1, 2, 4, 8, 16, 32],
                    help="downscale (1 = full 16384 px; default: 8 -> 2048 px)")
    ap.add_argument("--all", action="store_true", help="also duplicate maps")
    ap.add_argument("--list", action="store_true", help="list only")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    res = Res(a.res)
    tilesets = {}

    def tileset(n):
        if n not in tilesets:
            tilesets[n] = Tileset(res.get("BIN%02d.BIN" % n))
        return tilesets[n]

    names = [n[:-4] for n in sorted(res.idx) if n.endswith(".REF")]
    if a.maps:
        want = {m.upper().removesuffix(".REF") for m in a.maps}
        names = [n for n in names if n in want]
        missing = want - set(names)
        if missing:
            sys.exit("no such map: " + ", ".join(sorted(missing)))

    os.makedirs(a.out, exist_ok=True)
    seen = {}
    for name in names:
        blob = res.get(name + ".REF")
        orf_blob = res.get(name + ".ORF") if name + ".ORF" in res.idx else b""
        h = hashlib.md5(blob + orf_blob).hexdigest()   # same map, different objects: separate
        if h in seen and not (a.all or a.maps):
            print(f"{name:8}  = {seen[h]} (same map, skipped)")
            continue
        idx, codes = parse_ref(blob)
        orf = res.get(name + ".ORF") if name + ".ORF" in res.idx else b"\0"
        guess = (orf[0] & 0x0F) + 1
        scores = {b: seam_score(tileset(b), idx, codes) for b in range(1, 5)}
        bin_no = min(scores, key=scores.get)
        how = "ORF+fit" if bin_no == guess else f"fit (per ORF BIN{guess:02d})"
        print(f"{name:8}  BIN{bin_no:02d} {how}, border mismatch {scores[bin_no]:.1f}")
        seen.setdefault(h, name)
        if a.list:
            continue
        ts = tileset(bin_no)
        W, pix = compose(ts, idx, codes, a.scale)
        write_png(os.path.join(a.out, f"{name}.png"), W, W, pix, ts.pal)
        with open(os.path.join(a.out, f"{name}.json"), "w") as f:
            json.dump({"map": name, "tileset": f"BIN{bin_no:02d}",
                       "heightmap": f"DEPTH{bin_no:02d}.png", "shadow": f"SHADOW{bin_no:02d}.png",
                       "grid": GRID,
                       "tile_px": TILE, "tiles": list(idx), "orient": list(codes),
                       "objects": parse_orf(orf_blob) if orf_blob else []}, f)

    if not a.list:
        for n, ts in sorted(tilesets.items()):
            W, H = 16 * TILE, 8 * TILE
            img = bytearray(W * H)
            for k in range(NT):
                tx, ty = k % 16, k // 16
                for y in range(TILE):
                    p = (ty * TILE + y) * W + tx * TILE
                    img[p:p + TILE] = ts.tiles[k][y * TILE:(y + 1) * TILE]
            write_png(os.path.join(a.out, f"BIN{n:02d}_tileset.png"), W, H, bytes(img), ts.pal)
            with open(os.path.join(a.out, f"BIN{n:02d}_shade.bin"), "wb") as f:
                f.write(ts.shade)
            for kind in ("DEPTH", "SHADOW"):
                w, h, px, pal = decode_pcx(res.get(f"{kind}{n:02d}.PCX"))
                write_png(os.path.join(a.out, f"{kind}{n:02d}.png"), w, h, px, pal)
        # the viewer lists the maps from this
        index = []
        for fn in sorted(os.listdir(a.out)):
            if fn.endswith(".json") and fn != "index.json":
                with open(os.path.join(a.out, fn)) as f:
                    m = json.load(f)
                index.append({k: m[k] for k in ("map", "tileset", "heightmap", "shadow")})
        with open(os.path.join(a.out, "index.json"), "w") as f:
            json.dump(index, f, indent=1)
        print(f"Done: {a.out}")


if __name__ == "__main__":
    main()

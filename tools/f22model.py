#!/usr/bin/env python3
"""F-22 Lightning II 3DPK models (.PAK) export to OBJ + MTL + PNG.

Reverse-engineered format
----------------------
3DPK header (start of file)
  0x00  "3DPK"
  0x04  name (null-terminated, e.g. "A-50 RADAR PLANE")
  0x28  u32       number of levels of detail (LOD)
  0x40  LOD table, 8 bytes per level: u16 ?, u16 visibility distance, u32 offset
  LOD-blokk:  u16 0, u16 1, u32 part count, then 20 bytes per part:
              u32 offset of the 3DO1 object, 3 x i32 (always 0), i32 flag

3DO1 object (offsets relative to the start of the object)
  0x00  "3DO1", 0x04 01 01 00 00, 0x08 name[8] (space-padded)
  0x10  i32 bounding sphere radius, 6 x i32 bounding box (min, max pairs)
  0x2C  6 x i32  counts: texture, vertex, triangle, normal, t5, t6
  0x44  6 x i32  offsets of the tables (+ the end)
  texture  28 bytes: name[13], 08 08, flag, R, G, B (color if no name), ...,
           +22: blend flag: 0x10 = palette index 0 is transparent (fire, lights),
                0x80 = gray brightness map, 128 = unchanged, above it lightens,
                below it darkens (smoke, afterburner, sun, contrail); the other textures are
                opaque. The textures use their own PCX palette (the game
                maps them to the screen palette at load time).
  vertex   44 bytes: x, y, z (i32), 5 x i32 work area (0), u, v (16.16 fixed), 0
  triangle 32 bytes: i32 flag (0), texture, 3 vertex and 3 normal offsets
           (in bytes, from the start of the respective table)
  normal   16 bytes: nx, ny, nz (1.0 = 32768), 0
  point    12 bytes: x, y, z (i32) - attachment points on the fuselage: the 2nd, 3rd, ... part
           of the LOD is placed here (the parts are in local coordinates). The other parts'
           point tables are unfilled junk.

Coordinates: left-handed system; x = wingspan, y = longitudinal axis, z = up.
Unit ≈ 1/8192 ft (estimated from the F-22's wingspan and length).
In the OBJ: meters, right-handed, Y-up: (-x, z, -y). Because of the mirroring the
triangles keep their original order (in the game the front face is clockwise).
Verified on the Air Force One "UNITED STATES OF AMERICA"
lettering.

Usage:
  py tools/f22model.py                 # all models
  py tools/f22model.py F22 A50 SU27    # only these
  py tools/f22model.py --list
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f22res import decode_pcx, read_index, write_png  # noqa: E402

UNIT = 0.3048 / 8192   # meters / unit (estimate)


def cstr(b):
    return b.split(b"\0")[0].decode("ascii", "replace").strip()


class Obj3D:
    def __init__(self, d, at):
        assert d[at:at + 4] == b"3DO1", hex(at)
        self.name = cstr(d[at + 8:at + 16])
        h = struct.unpack_from("<19i", d, at + 0x10)
        self.radius = h[0]
        nt, nv, nf, nn = h[7:11]
        ot, ov, of, on = (at + x for x in h[13:17])
        self.textures = []
        for k in range(nt):
            r = d[ot + k * 28:ot + (k + 1) * 28]
            name, flags2 = cstr(r[:13]), r[22]
            mode = "opaque"
            if name and flags2 & 0x80:
                mode = "modulate"     # gray brightness map, 128 = unchanged
            elif name and flags2 & 0x10:
                mode = "cutout"       # palette index 0 is transparent
            self.textures.append({"name": name, "flags": r[15], "rgb": tuple(r[16:19]),
                                  "mode": mode, "frames": r[25], "delay": r[26],
                                  "raw": r[13:].hex()})
        # Animation: byte +25 of the first frame is the frame count (a power of 2), and if +26
        # is >= 127 the triangle's flag bit selects between the two frames (not
        # the time); otherwise the object's own counter (&(count-1)) selects. The other
        # frames follow it directly in the table, the triangles refer to the first.
        # (F22.EXE reverse-engineered: selection 0x40502; timer 120 Hz, the simulation runs in fixed
        # 40 Hz steps (main loop 0xF039, +3 ticks per step); the objects'
        # behavior is called by 0x53521 every step; the explosion advances every 5 steps.)
        for k, t in enumerate(self.textures):
            n = t["frames"]
            t["anim"] = self.textures[k:k + n] if n > 1 else None
        self.verts = []
        for k in range(nv):
            v = struct.unpack_from("<11i", d, ov + k * 44)
            self.verts.append((v[0], v[1], v[2], v[8] / 65536, v[9] / 65536))
        self.normals = [tuple(c / 32768 for c in struct.unpack_from("<3i", d, on + k * 16))
                        for k in range(nn)]
        self.faces = []
        for k in range(nf):
            f = struct.unpack_from("<8i", d, of + k * 32)
            self.faces.append((f[1] // 28, [x // 44 for x in f[2:5]], [x // 16 for x in f[5:8]]))
        n5, o5 = h[11], at + h[17]
        self.points = [struct.unpack_from("<3i", d, o5 + k * 12) for k in range(n5)]
        self.offset = (0, 0, 0)


class Model:
    def __init__(self, d):
        assert d[:4] == b"3DPK", "nem 3DPK"
        self.title = cstr(d[4:0x28])
        nl = struct.unpack_from("<I", d, 0x28)[0]
        self.lods = []
        for li in range(nl):
            _, dist, off = struct.unpack_from("<HHI", d, 0x40 + li * 8)
            n = struct.unpack_from("<I", d, off + 4)[0]
            parts = [Obj3D(d, struct.unpack_from("<I", d, off + 8 + p * 20)[0]) for p in range(n)]
            # The first part is the fuselage; the other parts (wings, landing gear, radome, ...) are in local
            # coordinates, their position is given by the fuselage attachment points, in order.
            for k, p in enumerate(parts[1:]):
                if k < len(parts[0].points):
                    p.offset = parts[0].points[k]
            self.lods.append((dist, parts))


def emit_texture(tex, mode, res_get, out_dir, tex_cache):
    """Writes the PNGs of a texture; returns: (MTL lines, {"map"|"modulate"|"color": ...})."""
    if not tex["name"]:
        rgb = [round(c / 255, 3) for c in tex["rgb"]]
        return [f"Kd {rgb[0]:.3f} {rgb[1]:.3f} {rgb[2]:.3f}"], {"color": rgb}
    stem = tex["name"].rsplit(".", 1)[0].lower()
    if stem not in tex_cache:
        try:
            tex_cache[stem] = decode_pcx(res_get(tex["name"]))
        except KeyError:
            tex_cache[stem] = None
    src = tex_cache[stem]
    if not src:
        return ["Kd 1 0 1  # missing texture: " + tex["name"]], {"color": [1, 0, 1]}
    if mode == "opaque":
        write_png(os.path.join(out_dir, stem + ".png"), *src)
        return ["Kd 1 1 1", f"map_Kd {stem}.png"], {"map": stem + ".png"}
    if mode == "cutout":
        # paletted PNG, index 0 is transparent (tRNS)
        png = stem + "_cut.png"
        write_png(os.path.join(out_dir, png), *src, trns=[0] + [255] * 255)
        return ["Kd 1 1 1", f"map_Kd {png}", f"map_d {png}"], {"map": png}
    # In the game: screen = screen * index / 128 (128 = transparent).
    # Exact version: gray intensity map (*_mod.png, for "2x modulate" blending);
    # alpha approximation for generic OBJ viewers (*_add.png): white above 128, below it
    # black, with alpha proportional to the deviation.
    w, h, px, _ = src
    grey = bytes(c for v in range(256) for c in (v, v, v))
    write_png(os.path.join(out_dir, stem + "_mod.png"), w, h, px, grey)
    pal = bytes(c for v in range(256) for c in ((255,) * 3 if v >= 128 else (0,) * 3))
    alpha = [min(255, abs(v - 128) * 2) for v in range(256)]
    write_png(os.path.join(out_dir, stem + "_add.png"), w, h, px, pal, trns=alpha)
    return (["Kd 1 1 1", f"map_Kd {stem}_add.png", f"map_d {stem}_add.png"],
            {"map": stem + "_add.png", "modulate": stem + "_mod.png"})


def export(model, name, res_get, out_dir, tex_cache):
    os.makedirs(out_dir, exist_ok=True)
    written = []
    mat_info = {}   # material name -> blend mode; the viewer reads it from here
    for li, (dist, parts) in enumerate(model.lods):
        base = f"{name}_lod{li}"
        mats = {}
        lines = [f"# {model.title} – LOD {li} (visibility: {dist})", f"mtllib {base}.mtl"]
        vbase = nbase = tbase = 1
        for p in parts:
            lines.append(f"o {p.name}")
            ox, oy, oz = p.offset
            for x, y, z, u, v in p.verts:
                x, y, z = x + ox, y + oy, z + oz
                lines.append(f"v {-x * UNIT:.5f} {z * UNIT:.5f} {-y * UNIT:.5f}")
            for x, y, z, u, v in p.verts:
                lines.append(f"vt {u:.5f} {1 - v:.5f}")
            for nx, ny, nz in p.normals:
                lines.append(f"vn {-nx:.4f} {nz:.4f} {-ny:.4f}")
            cur = None
            for t, vs, ns in p.faces:
                tex = p.textures[t]
                key = tex["name"] or "col_%02x%02x%02x" % tex["rgb"]
                key = key.replace(".PCX", "").lower()
                if tex["mode"] != "opaque":
                    key += "_" + tex["mode"]
                if tex["anim"]:
                    key += f"_anim{len(tex['anim'])}_{tex['delay']}"
                mats[key] = tex
                if key != cur:
                    lines.append(f"usemtl {key}")
                    cur = key
                if p.normals:
                    lines.append("f " + " ".join(f"{vbase + a}/{tbase + a}/{nbase + n}" for a, n in zip(vs, ns)))
                else:
                    lines.append("f " + " ".join(f"{vbase + a}/{tbase + a}" for a in vs))
            vbase += len(p.verts)
            tbase += len(p.verts)
            nbase += len(p.normals)
        mtl = []
        for key, tex in sorted(mats.items()):
            mtl.append(f"newmtl {key}")
            mode = tex["mode"]
            lines_, info = emit_texture(tex, mode, res_get, out_dir, tex_cache)
            mtl += lines_
            info["mode"] = mode
            if tex["anim"]:
                # per frame in the same blend mode (the viewer swaps them)
                info["anim"] = {"delay": tex["delay"],
                                "frames": [emit_texture(fr, mode, res_get, out_dir, tex_cache)[1]
                                           for fr in tex["anim"]]}
            if tex["flags"] & 0x80:
                info["flag80"] = True
            mat_info[key] = info
            mtl.append("")
        with open(os.path.join(out_dir, base + ".obj"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        with open(os.path.join(out_dir, base + ".mtl"), "w", encoding="utf-8") as f:
            f.write("\n".join(mtl) + "\n")
        written.append((li, dist, len(parts), sum(len(p.faces) for p in parts)))
    import json
    with open(os.path.join(out_dir, "materials.json"), "w", encoding="utf-8") as f:
        json.dump(mat_info, f, indent=1)
    return written


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="*", help="model names without .PAK, e.g. F22 SU27")
    ap.add_argument("--res", default=os.path.join(root, "RESOURCE.RES"))
    ap.add_argument("-o", "--out", default=os.path.join(root, "extracted", "models"))
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    with open(a.res, "rb") as f:
        data = f.read()
    idx = {n.upper(): (o, s) for n, o, s in read_index(data)}

    def get(n):
        o, s = idx[n.upper()]
        return data[o:o + s]

    names = sorted(n[:-4] for n in idx if n.endswith(".PAK"))
    if a.models:
        want = {m.upper().removesuffix(".PAK") for m in a.models}
        missing = want - set(names)
        if missing:
            sys.exit("nincs ilyen modell: " + ", ".join(sorted(missing)))
        names = [n for n in names if n in want]

    tex_cache = {}
    catalog = []
    for n in names:
        m = Model(get(n + ".PAK"))
        if a.list:
            lods = ", ".join(f"{len(p)} parts/{sum(len(o.faces) for o in p)} triangles"
                             for _, p in m.lods)
            print(f"{n:10} {m.title:28} {lods}")
            continue
        info = export(m, n, get, os.path.join(a.out, n), tex_cache)
        print(f"{n:10} {m.title:28} " + "  ".join(f"lod{li}:{f}△" for li, _, _, f in info))
        catalog.append({"name": n, "title": m.title,
                        "lods": [{"dist": dist, "parts": np, "tris": f} for _, dist, np, f in info]})
    if not a.list:
        # the viewer lists from this; on a partial export we keep the existing entries
        import json
        path = os.path.join(a.out, "index.json")
        old = []
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                old = [e for e in json.load(f) if e["name"] not in {c["name"] for c in catalog}]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(sorted(old + catalog, key=lambda e: e["name"]), f, indent=1, ensure_ascii=False)
        print(f"Done: {a.out}")


if __name__ == "__main__":
    main()

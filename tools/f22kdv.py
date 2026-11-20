#!/usr/bin/env python3
"""F-22 Lightning II (NovaLogic) KDV video decoder / converter.

Format (reverse-engineered from the F22.EXE routines at 0x89700-0x8a301; container at 0x89b6f, frame decoder at 0x89f6d):
  header  'xVDK': u32 header size (28), width, height, frame count, timing (120 Hz / n, 8 = 15 fps,
          10 = 12 fps), 34 (version)
  followed by chunks: u32 tag, u32 FULL size (including the header):
    'pVDK'  palette: 768 bytes (256 x RGB, 6-bit VGA values)
    'iVDK'  one frame: 4x4 blocks, 2-bit commands (four per byte, LSB first)
    'sVDK'  audio, two 'SAMP' pieces (left, right): u32 'SAMP', u32 length, 8 bytes of parameters, then 'length' bytes
            8-bit unsigned PCM, 22050 Hz
    'aVDK'  audio, one 'SAMP' piece (mono), the chunk aligned to 4 bytes
  Image block commands (color index 0 is transparent: it does not overwrite that pixel of the block):
    0: 4 colors + 4 bytes (16 2-bit selectors, LSB first)
    1: 2 color bytes + 2 mask bytes (16 bits); if the two color bytes are identical: 16 raw pixels after the 2 bytes
    2: 1 color, fills the block
    3: repeat the previous block's mode with the same color / palette (0: new selectors, 1: new mask, 2: fill)

Usage:
  py tools/f22kdv.py info  file.kdv
  py tools/f22kdv.py frame file.kdv N output.png
  py tools/f22kdv.py wav   file.kdv output.wav
  py tools/f22kdv.py mp4   file.kdv output.mp4      (needs ffmpeg)
"""
import struct
import sys
import wave

TAG = {b'xVDK': 'head', b'pVDK': 'pal', b'iVDK': 'img', b'sVDK': 'sst', b'aVDK': 'mono'}


class Kdv:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:4] != b'xVDK':
            raise ValueError('not a KDV file')
        (self.hs, self.w, self.h, self.frames, self.div, self.ver) = struct.unpack_from('<6I', d, 4)
        self.fps = 120.0 / self.div

    def chunks(self):
        d = self.d
        p = self.hs
        while p + 8 <= len(d):
            tag = d[p:p + 4]
            sz = struct.unpack_from('<I', d, p + 4)[0]
            if sz < 8:
                break
            yield tag, p, sz
            p += sz

    def audio(self):
        """(left, right) 8-bit PCM bytes; for a mono file both are the same."""
        d = self.d
        L = bytearray()
        R = bytearray()
        for tag, p, sz in self.chunks():
            if tag == b'sVDK':
                q, k = p + 8, 0
                while q + 16 <= p + sz:
                    ln = struct.unpack_from('<I', d, q + 4)[0]
                    (L if k == 0 else R).extend(d[q + 16:q + 16 + ln])
                    q += 16 + ln
                    k += 1
            elif tag == b'aVDK':
                ln = struct.unpack_from('<I', d, p + 12)[0]
                L.extend(d[p + 24:p + 24 + ln])
        if not R:
            R = L
        return bytes(L), bytes(R)


def decode_frame(buf, w, h, data, st):
    """Decode one frame into the buf (w*h bytes, persistent) array. st: {'mode': .., 'pal': ..} state."""
    esi = 0
    bl = 0
    bh = 0
    mode = st.get('mode', 0)
    pal = st.get('pal', 0)
    n = len(data)

    def put(x0, y0, vals):
        for i, v in enumerate(vals):
            if v:
                buf[(y0 + (i >> 2)) * w + x0 + (i & 3)] = v

    for by in range(h // 4):
        for bx in range(w // 4):
            if (bh & 3) == 0:
                bl = data[esi] if esi < n else 0
                esi += 1
            bh += 1
            cmd = bl & 3
            x0, y0 = bx * 4, by * 4
            if cmd == 3:
                if mode == 2:
                    c = data[pal]
                    if c:
                        put(x0, y0, [c] * 16)
                elif mode == 0:
                    sel = struct.unpack_from('<I', data, esi)[0]
                    put(x0, y0, [data[pal + ((sel >> (2 * i)) & 3)] for i in range(16)])
                    esi += 4
                else:
                    mask = data[esi] | data[esi + 1] << 8
                    put(x0, y0, [data[pal + ((mask >> i) & 1)] for i in range(16)])
                    esi += 2
            elif cmd == 2:
                mode = 2
                pal = esi
                esi += 1
                c = data[pal]
                if c:
                    put(x0, y0, [c] * 16)
            elif cmd == 0:
                mode = 0
                pal = esi
                esi += 4
                sel = struct.unpack_from('<I', data, esi)[0]
                put(x0, y0, [data[pal + ((sel >> (2 * i)) & 3)] for i in range(16)])
                esi += 4
            else:
                if data[esi] == data[esi + 1]:
                    put(x0, y0, data[esi + 2:esi + 18])
                    esi += 18
                else:
                    mode = 1
                    pal = esi
                    esi += 2
                    mask = data[esi] | data[esi + 1] << 8
                    put(x0, y0, [data[pal + ((mask >> i) & 1)] for i in range(16)])
                    esi += 2
            bl >>= 2
    st['mode'] = mode
    st['pal'] = pal


def frames(k):
    """Generator: (index, 8-bit image bytes, palette [768 bytes, 8-bit RGB])."""
    buf = bytearray(k.w * k.h)
    pal = bytes(768)
    st = {}
    i = 0
    d = k.d
    for tag, p, sz in k.chunks():
        if tag == b'pVDK':
            raw = d[p + 8:p + 8 + 768]
            pal = bytes(min(255, v << 2 | v >> 4) for v in raw) if max(raw) < 64 else bytes(raw)
        elif tag == b'iVDK':
            decode_frame(buf, k.w, k.h, d[p + 8:p + sz], st)
            yield i, bytes(buf), pal
            i += 1


def to_png(path, w, h, px, pal):
    import zlib
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            c = px[y * w + x]
            rows += pal[c * 3:c * 3 + 3]

    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


def main():
    a = sys.argv
    if len(a) < 3:
        print(__doc__)
        return
    k = Kdv(a[2])
    if a[1] == 'info':
        print(f'{k.w}x{k.h} {k.frames} frames, {k.fps:g} fps, {k.frames / k.fps:.1f} s')
        import collections
        print(collections.Counter(t for t, _, _ in k.chunks()))
    elif a[1] == 'frame':
        n = int(a[3])
        for i, px, pal in frames(k):
            if i == n:
                to_png(a[4], k.w, k.h, px, pal)
                break
    elif a[1] == 'wav':
        L, R = k.audio()
        w = wave.open(a[3], 'wb')
        w.setnchannels(2)
        w.setsampwidth(1)
        w.setframerate(22050)
        inter = bytearray(len(L) * 2)
        inter[0::2] = L
        inter[1::2] = R[:len(L)].ljust(len(L), b'\x80')
        w.writeframes(bytes(inter))
        w.close()
    elif a[1] == 'mp4':
        import subprocess
        import tempfile
        import os
        tmp = tempfile.mkdtemp()
        wav = os.path.join(tmp, 'a.wav')
        L, R = k.audio()
        w = wave.open(wav, 'wb')
        w.setnchannels(2)
        w.setsampwidth(1)
        w.setframerate(22050)
        inter = bytearray(len(L) * 2)
        inter[0::2] = L
        inter[1::2] = R[:len(L)].ljust(len(L), b'\x80')
        w.writeframes(bytes(inter))
        w.close()
        p = subprocess.Popen(['ffmpeg', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{k.w}x{k.h}', '-r', f'{k.fps}',
                              '-i', '-', '-i', wav, '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest', a[3]],
                             stdin=subprocess.PIPE)
        for i, px, pal in frames(k):
            p.stdin.write(b''.join(pal[c * 3:c * 3 + 3] for c in px))
        p.stdin.close()
        p.wait()


if __name__ == '__main__':
    main()

"""Assemble a multi-size .ico from PNG frames.

Icons store width/height as single bytes (0 means 256) and each directory entry
is 16 bytes: w, h, colors, reserved, planes(2), bitcount(2), bytes(4), offset(4).
The earlier attempt wrote bitcount as 32 (wrong width in the colour-count byte
position) which produced a corrupt directory that windres rejected.
"""
import os
import struct
import sys

def build(frames_dir, out_path):
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = []
    for s in sizes:
        p = os.path.join(frames_dir, f"{s}.png")
        with open(p, "rb") as f:
            frames.append(f.read())

    header = struct.pack("<HHH", 0, 1, len(sizes))
    entries = b""
    offset = 6 + 16 * len(sizes)
    payload = b""
    for s, data in zip(sizes, frames):
        w = 0 if s >= 256 else s
        h = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        payload += data

    with open(out_path, "wb") as f:
        f.write(header + entries + payload)
    print(f"wrote {out_path}: {os.path.getsize(out_path)} bytes, {len(sizes)} sizes")

if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])

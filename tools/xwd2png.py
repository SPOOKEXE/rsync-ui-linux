#!/usr/bin/env python3
"""Converts X11 window dumps (xwd) to PNG, so a headless run can be looked at.

xwd is the only capture tool that works on a bare Xvfb with no desktop session,
and nothing else here reads its format.

  xwd2png.py in.xwd out.png
  xwd2png.py --best out.png a.xwd b.xwd ...   pick the frame with real content
"""
import struct
import sys

from PIL import Image


def decode(path):
    data = open(path, "rb").read()
    f = struct.unpack(">25I", data[:100])
    hdr_size, depth, width, height = f[0], f[3], f[4], f[5]
    bits_per_pixel, bytes_per_line, ncolors = f[11], f[12], f[19]

    if bits_per_pixel != 32:
        raise SystemExit(f"expected 32 bpp, got {bits_per_pixel} (depth {depth})")

    # A colormap of ncolors 12-byte XWDColor entries sits between the header and
    # the pixels, even for a TrueColor visual where it carries nothing useful.
    # Skipping it rolls every row sideways by 768 pixels, which looks
    # convincingly like tearing.
    start = hdr_size + 12 * ncolors
    if start + bytes_per_line * height != len(data):
        raise SystemExit(f"size mismatch in {path}")

    img = Image.new("RGB", (width, height))
    out = img.load()
    for y in range(height):
        row = data[start + y * bytes_per_line:start + (y + 1) * bytes_per_line]
        for x in range(width):
            b, g, r, _a = row[x * 4:x * 4 + 4]
            out[x, y] = (r, g, b)
    return img


def content_score(img):
    """How much of the frame is not flat black. A blank grab scores zero."""
    return sum(1 for p in img.getdata() if p != (0, 0, 0))


def main():
    if sys.argv[1] == "--best":
        dest, sources = sys.argv[2], sys.argv[3:]
        best, best_score = None, -1
        for path in sources:
            img = decode(path)
            score = content_score(img)
            if score > best_score:
                best, best_score = img, score
        best.save(dest)
        print(f"wrote {dest} {best.size[0]}x{best.size[1]} "
              f"({best_score} non-black pixels, best of {len(sources)})")
    else:
        img = decode(sys.argv[1])
        img.save(sys.argv[2])
        print(f"wrote {sys.argv[2]} {img.size[0]}x{img.size[1]}")


if __name__ == "__main__":
    main()

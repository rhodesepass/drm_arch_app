#!/usr/bin/env python3
"""
Convert PNG/JPG/GIF to .splash format for ArkEPass boot splash.

Static image:
  [u16 width][u16 height][RGB565 pixels]

Animated GIF:
  [4B "ESPL"][u16 width][u16 height][u16 frames][u16 delay_ms]
  [frames * RGB565 pixels]

Transparent pixels are composited against BLACK (not white).

Usage:
  python3 png2splash.py image.png                       # static
  python3 png2splash.py image.png -o splash.splash      # static, custom output
  python3 png2splash.py image.png --resize 300x300      # static, resized
  python3 png2splash.py anim.gif                        # animated GIF
  python3 png2splash.py anim.gif --max-frames 30        # limit frame count
"""

import sys
import struct
import os

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow not installed. Run: pip install Pillow")
    sys.exit(1)

def rgb_to_565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def to_rgb_black_bg(img):
    """Convert image to RGB, compositing alpha against BLACK background.

    Pillow's img.convert("RGB") composites against WHITE for RGBA,
    and IGNORES the transparency index entirely for P-mode palette images.
    Both produce white pixels where transparency should be black.

    Fix: always go through RGBA first. Pillow's P→RGBA conversion correctly
    maps the palette transparency index to alpha=0, then we composite on black.
    """
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    bg = Image.new("RGB", img.size, (0, 0, 0))
    bg.paste(img, mask=img.split()[3])
    return bg

def frame_to_rgb565(img):
    """Convert PIL Image to RGB565 bytes."""
    img = to_rgb_black_bg(img)
    w, h = img.size
    data = bytearray(w * h * 2)
    pixels = img.load()
    idx = 0
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            v = rgb_to_565(r, g, b)
            data[idx] = v & 0xFF
            data[idx + 1] = (v >> 8) & 0xFF
            idx += 2
    return bytes(data)

def convert_static(img, output, resize=None):
    if resize:
        img = img.resize(resize, Image.LANCZOS)
    w, h = img.size
    with open(output, "wb") as f:
        f.write(struct.pack("<HH", w, h))
        f.write(frame_to_rgb565(img))
    print(f"Static: {w}x{h} -> {output} ({os.path.getsize(output)} bytes)")

def convert_gif(img, output, resize=None, max_frames=100):
    frames = []
    delays = []
    try:
        while True:
            frame = img.copy()
            if resize:
                frame = frame.resize(resize, Image.LANCZOS)
            frames.append(frame)
            delays.append(img.info.get("duration", 100))
            if len(frames) >= max_frames:
                break
            img.seek(img.tell() + 1)
    except EOFError:
        pass

    if not frames:
        print("Error: no frames found")
        sys.exit(1)

    avg_delay = max(10, sum(delays) // len(delays))
    w, h = frames[0].size
    n = len(frames)

    with open(output, "wb") as f:
        f.write(b"ESPL")
        f.write(struct.pack("<HHHH", w, h, n, avg_delay))
        for frame in frames:
            f.write(frame_to_rgb565(frame))

    size = os.path.getsize(output)
    print(f"Animated: {w}x{h}, {n} frames, {avg_delay}ms delay -> {output} ({size} bytes, {size/1024:.0f}KB)")

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    inp = args[0]
    output = None
    resize = None
    max_frames = 100

    i = 1
    while i < len(args):
        if args[i] in ("-o", "--output") and i + 1 < len(args):
            output = args[i + 1]; i += 2
        elif args[i] == "--resize" and i + 1 < len(args):
            parts = args[i + 1].lower().split("x")
            resize = (int(parts[0]), int(parts[1])); i += 2
        elif args[i] == "--max-frames" and i + 1 < len(args):
            max_frames = int(args[i + 1]); i += 2
        else:
            i += 1

    if not output:
        output = os.path.splitext(inp)[0] + ".splash"

    img = Image.open(inp)
    is_gif = getattr(img, "is_animated", False)

    if is_gif:
        convert_gif(img, output, resize, max_frames)
    else:
        convert_static(img, output, resize)

if __name__ == "__main__":
    main()

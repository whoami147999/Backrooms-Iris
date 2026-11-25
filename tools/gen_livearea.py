#!/usr/bin/env python3
"""Regenerate the PS Vita LiveArea images from real in-game renders.

    python3 tools/gen_livearea.py build/backrooms-iris

Needs: python-pillow, a PC build of the game, and a display (or xvfb-run).
The Vita only accepts 8-bit *indexed* PNGs for these files, which this script handles.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "vita", "sce_sys")
FONTS = [
    "/usr/share/fonts/TTF/JetBrainsMono-Bold.ttf",                    # Arch: ttf-jetbrains-mono
    "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
]


def font(size):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def render(game, w, h, level, *extra):
    fd, path = tempfile.mkstemp(suffix=".bmp")
    os.close(fd)
    cmd = [game, "--level", str(level), "--seed", "1215", "--nohud", "--after", "6",
           "--shot", path] + list(extra)
    env = dict(os.environ, HOME=tempfile.mkdtemp())  # don't touch the real settings file
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, env=env)
    img = Image.open(path).convert("RGB")
    img.load()
    os.remove(path)
    return img.resize((w, h), Image.LANCZOS)


def vhs(img, strength=1.0):
    """Lo-fi pass: slight chroma shift, scanlines, vignette."""
    r, g, b = img.split()
    r = r.transform(img.size, Image.AFFINE, (1, 0, -1 * strength, 0, 1, 0))
    b = b.transform(img.size, Image.AFFINE, (1, 0, 1 * strength, 0, 1, 0))
    img = Image.merge("RGB", (r, g, b))
    d = ImageDraw.Draw(img, "RGBA")
    for y in range(0, img.height, 2):
        d.line([(0, y), (img.width, y)], fill=(0, 0, 0, int(28 * strength)))
    vig = Image.new("L", img.size, 0)
    vd = ImageDraw.Draw(vig)
    vd.ellipse([-img.width * 0.25, -img.height * 0.3, img.width * 1.25, img.height * 1.3], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(img.width // 10))
    return Image.composite(img, Image.new("RGB", img.size, (8, 6, 2)), vig)


def title(img, text, size, y, sub=None):
    d = ImageDraw.Draw(img, "RGBA")
    f = font(size)
    tw = d.textlength(text, font=f)
    x = (img.width - tw) / 2
    d.text((x + 2, y + 2), text, font=f, fill=(0, 0, 0, 200))
    d.text((x - 1, y), text, font=f, fill=(255, 60, 40, 120))   # red ghost (VHS bleed)
    d.text((x, y), text, font=f, fill=(240, 228, 170, 255))
    if sub:
        fs = font(max(10, size // 3))
        sw = d.textlength(sub, font=fs)
        d.text(((img.width - sw) / 2, y + size + 4), sub, font=fs, fill=(210, 200, 150, 220))
    return img


def save_indexed(img, rel):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("P", palette=Image.ADAPTIVE, colors=256).save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT), img.size)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    bg = vhs(render(game, 840, 500, 5, "--yaw", "0"), 1.2)                       # Level 5 corridor
    save_indexed(bg, "livearea/contents/bg.png")

    st = vhs(render(game, 280, 158, 0, "--yaw", "23"), 0.6)                     # Level 0
    save_indexed(title(st, "BACKROOMS IRIS", 26, 58, "co-op horror"), "livearea/contents/startup.png")

    ic = vhs(render(game, 128, 128, 8, "--yaw", "200", "--walk", "1.5", "--entity", "2", "--dist", "2.2"), 0.5)
    save_indexed(title(ic, "IRIS", 30, 92), "icon0.png")                         # a Smiler in the caves

    with open(os.path.join(OUT, "livearea", "contents", "template.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')
    print("wrote vita/sce_sys/livearea/contents/template.xml")


if __name__ == "__main__":
    main()

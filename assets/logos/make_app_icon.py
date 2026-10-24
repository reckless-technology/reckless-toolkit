"""The Standalone apps' icon, RecklessAppIcon.png (1024 px), RecklessAppIcon256.png and
RecklessAppIcon64.png: the
three-dimensional company logo - the red roundel with the bevelled white R and the
superscript t - on transparency, centred on a square canvas.

Run by hand, not by the build: the source drawing lives in another repository
(reckless-technology/hardware, libraries/RTL logos/Reckless3Dlogo.jpg, 323 x 303) and the
logo changes about never, so the PNGs are committed as the header's mark is (make_mark.py,
from the same repository's 2D drawing, whose method this is).

    gh api -H "Accept: application/vnd.github.raw" \\
        "repos/reckless-technology/hardware/contents/libraries/RTL%20logos/Reckless3Dlogo.jpg" \\
        > Reckless3Dlogo.jpg
    python3 make_app_icon.py    # writes the three PNGs beside it; delete the JPEG after

The bundles' recipes (cmake/RecklessBundle.cmake) make the macOS bundle's Icon.icns and the
Windows executable's and VST3's .ico from the 1024 (app/make_app_icons.py); the
Standalone app carries the 256 and sets it as its window's icon on Linux (app/linux/
AppLinux.cpp), where a desktop takes a program's icon from its window and no file carries one;
the 64 is the logo at the top of every page of the source documentation (the root Doxyfile).

The two traps make_mark.py documents, handled the same way:

  * The R is white and ENCLOSED by the red, and its bevel is grey: the background is the
    near-white region CONNECTED TO THE BORDER, found by a scanline fill from the corner, so
    the letter and its shading, unreachable from outside, stay opaque.
  * Every pixel on the logo's outer boundary is red - the silhouette is the two red discs -
    so the ground is repainted red before the alpha is applied, or the half-transparent edge
    would carry the source's white ground as a pale fringe on a dark dock or taskbar.

The outer edge's alpha is the source's own antialiasing, read as red coverage (white is
G = 255, the red is G = 6). The 1024 is an upscale of a 323-pixel drawing, soft only where an
icon is shown larger than about 300 px; the 256 and the 64 are downscales.
"""
import numpy as np
from PIL import Image

SRC    = "Reckless3Dlogo.jpg"
SIZES  = {"RecklessAppIcon.png": 1024, "RecklessAppIcon256.png": 256, "RecklessAppIcon64.png": 64}
FILL   = 0.90    # the logo's longest side as a share of the canvas: a margin the platforms expect
RED    = np.array([206, 6, 6])

a = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
h, w, _ = a.shape
loose_white = a.min(axis=2) > 200          # loose, so the fill takes the whole edge ramp

# ---- scanline flood fill from the corner: the ground, and only the ground ----------
bg = np.zeros((h, w), bool)
stack = [(0, 0)]
while stack:
    x, y = stack.pop()
    if bg[y, x] or not loose_white[y, x]:
        continue
    xl = x
    while xl > 0 and loose_white[y, xl - 1] and not bg[y, xl - 1]:
        xl -= 1
    xr = x
    while xr < w - 1 and loose_white[y, xr + 1] and not bg[y, xr + 1]:
        xr += 1
    bg[y, xl:xr + 1] = True
    for ny in (y - 1, y + 1):
        if 0 <= ny < h:
            for nx in range(xl, xr + 1):
                if loose_white[ny, nx] and not bg[ny, nx]:
                    stack.append((nx, ny))
print(f"ground {int(bg.sum())} px, logo {int((~bg).sum())} px of {bg.size}")

# ---- alpha: opaque inside the silhouette, the source's own ramp on the outer edge --
coverage = np.clip((255.0 - a[..., 1]) / (255.0 - RED[1]), 0.0, 1.0)
alpha = np.where(~bg, 1.0, coverage)

# ---- colour: the ground repainted red so the edge blends red, never white ----------
rgb = a.copy()
rgb[bg] = RED

# ---- crop to the logo, then centre it on each square canvas ------------------------
ys, xs = np.nonzero(alpha > 0.02)
box = (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)
cw, ch = box[2] - box[0], box[3] - box[1]
logo_rgb = Image.fromarray(rgb.astype(np.uint8)).crop(box)
logo_alpha = Image.fromarray((alpha * 255).round().astype(np.uint8)).crop(box)
print(f"logo {cw}x{ch}")

for name, side in SIZES.items():
    scale = FILL * side / max(cw, ch)
    size = (max(1, round(cw * scale)), max(1, round(ch * scale)))
    logo = logo_rgb.resize(size, Image.LANCZOS).convert("RGBA")
    logo.putalpha(logo_alpha.resize(size, Image.LANCZOS))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(logo, ((side - size[0]) // 2, (side - size[1]) // 2), logo)
    canvas.save(name)

    c = np.asarray(canvas).astype(int)
    opaque_white = (c[..., 0] > 230) & (c[..., 1] > 230) & (c[..., 2] > 230) & (c[..., 3] > 200)
    print(f"{name}: {side}x{side}, logo {size[0]}x{size[1]}; CHECK corner alpha {c[0, 0, 3]} (want 0), "
          f"centre alpha {c[side // 2, side // 2, 3]} (want 255), opaque white px {int(opaque_white.sum())} (want > 0)")

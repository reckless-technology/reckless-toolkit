"""The company mark, RecklessRtLogo.png: the red roundel with the R knocked out in white
and the superscript t beside it, on transparency - no disc, no rim.

Run by hand, not by the build: the source drawing lives in another repository
(reckless-technology/hardware, libraries/RTL logos/Reckless2Dlogo.jpg) and the mark
changes about never, so the PNG is committed as the knob strip and the meter artwork are.

    python3 make_mark.py        # writes RecklessRtLogo.png beside it, from that JPEG

Two traps, both handled here:

  * The R is white and ENCLOSED by the red. A plain "white becomes transparent" rule
    punches the letter out and leaves the panel showing through it. The background is
    therefore the near-white region CONNECTED TO THE BORDER, found by a scanline fill
    from the corner; the letter, unreachable from outside, stays opaque white. (PIL's
    own ImageDraw.floodfill is a no-op on an image built from a numpy buffer, and
    scipy is not available here, so the fill is written out.)
  * Every pixel on the mark's outer boundary is red - the silhouette is the two red
    discs - so the ground is repainted red before the alpha is applied. Without that,
    the half-transparent edge pixels carry the source's white ground and the mark
    renders with a pale fringe on a dark panel.

The outer edge's alpha is the source's OWN antialiasing, read as red coverage
(white is G=255, the red is G=6), not a threshold: the fill deliberately runs on a
loose white so it eats the whole ramp, and coverage then lays the ramp back down.
"""
import numpy as np
from PIL import Image

SRC   = "Reckless2Dlogo.jpg"
FINAL = 512      # the asset's longest side
PAD   = 0.01     # a whisker of transparent margin so the edge never clips
RED   = np.array([206, 6, 6])

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
print(f"ground {int(bg.sum())} px, mark {int((~bg).sum())} px of {bg.size}")

# ---- alpha: opaque inside the silhouette, the source's own ramp on the outer edge --
coverage = np.clip((255.0 - a[..., 1]) / (255.0 - RED[1]), 0.0, 1.0)
alpha = np.where(~bg, 1.0, coverage)

# ---- colour: the ground repainted red so the edge blends red, never white ----------
rgb = a.copy()
rgb[bg] = RED

# ---- crop to the mark, a whisker of margin, then size to FINAL ---------------------
ys, xs = np.nonzero(alpha > 0.02)
x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
pad = int(round(PAD * max(x1 - x0, y1 - y0)))
x0, y0 = max(0, x0 - pad), max(0, y0 - pad)
x1, y1 = min(w - 1, x1 + pad), min(h - 1, y1 + pad)
cw, ch = x1 - x0 + 1, y1 - y0 + 1
scale = FINAL / max(cw, ch)
size = (max(1, round(cw * scale)), max(1, round(ch * scale)))
print(f"crop {cw}x{ch} -> asset {size[0]}x{size[1]}, aspect {size[0]/size[1]:.3f}")

box = (x0, y0, x1 + 1, y1 + 1)
out = Image.fromarray(rgb.astype(np.uint8)).crop(box).resize(size, Image.LANCZOS).convert("RGBA")
out.putalpha(Image.fromarray((alpha * 255).astype(np.uint8)).crop(box).resize(size, Image.LANCZOS))
out.save("RecklessRtLogo.png")

c = np.asarray(out).astype(int)
opaque_white = (c[..., 0] > 230) & (c[..., 1] > 230) & (c[..., 2] > 230) & (c[..., 3] > 200)
print(f"CHECK corner alpha {c[0,0,3]} (want ~0)   centre alpha {c[size[1]//2, size[0]//2, 3]} (want 255)")
print(f"CHECK opaque white px, the R and the t: {int(opaque_white.sum())} (want > 0)")

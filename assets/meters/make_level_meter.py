#!/usr/bin/env python3
"""Generates the three layers of the series' small IN and OUT level meter (panel/'s LevelMeterView, the
Pedalboard's two):

    assets/meters/level_meter_face.svg    static: bezel, backlit face, arc, ticks, red zone (compiled into the plugin)
    assets/meters/level_meter_needle.svg  rotate the <g id="needle"> about (500, 880) (the needle's source; the
                                   panel draws it as a path, panel/'s ArtworkMeterView)
    assets/meters/level_meter_glass.svg   static: glass, window rim, bezel screws - drawn ABOVE the needle (compiled)

Same 1000 x 1000 canvas, housing rect (40,140 920x720), pivot and +/-34 degree sweep as vu_meter_*.svg (the
gain-reduction meter), so the layers go through the same view (panel/'s ArtworkMeterView) and scale with the
same aspect ratio (1.278). The meter is shown at 0.45x the GR meter's height, two of them stacked with a gap of 0.10x
so the pair matches the GR meter, so tick strokes are ~1.5x heavier to stay legible at that size.

Scale: -30 dB ... +6 dB. Needle deflection follows a half-power law (sqrt of linear amplitude), which puts 0 dB
at 2/3 of the sweep like a classic VU face and spreads the quiet end more evenly than a pure voltage law. The same
mapping is look::LevelMeterScale::Position / AngleDegrees in look/LookMeters.h. The face's decibels are the
meter's own, not dBFS: the panel reads them 6 dB over the digital level (panel::LevelMeterView::kZeroDbfs, -6 dBFS
on the 0 and full scale on the +6) as a peak programme reading, instant up and an RC discharge down with a 750 ms
time constant (panel::LevelMeterView::kFallSeconds).

The scale's figures (-30 -20 -10 -3 0 +3 +6) and the legend (IN or OUT with "PEAK" under it) are not in the SVG: the
panel draws them in the series' typeface at the positions below (look::LevelMeterScale::Paint, through
LevelMeterView::PaintScale) - as the gain-reduction meter's are - so the two meters' faces are set in one face.
FIGURE_RADIUS, FIGURE_SIZE, LEGEND_Y and LEGEND_SIZE are where the artwork places them, mirrored by the constants in
look/LookMeters.cpp (kLevelFigureRadius and the rest).

    python3 assets/meters/make_level_meter.py
"""

import argparse
import math
import os

# ----------------------------------------------------------------- geometry
PIVOT = (500.0, 880.0)
SWEEP = 34.0            # degrees either side of vertical
ARC_R = 560.0
DB_MIN, DB_MAX = -30.0, 6.0
INK = "#1a1005"         # the GR meter's tick/arc colour
RED = "#c8102e"         # the GR meter's needle tip red

MAJOR = [-30, -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3, 6]
MINOR = [-25, -15, -8, -6, -4, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 4, 5]

# Where the panel draws the figures and the legend, in the artwork's units (look/LookMeters.cpp mirrors these).
FIGURES = [-30, -20, -10, -3, 0, 3, 6]
FIGURE_RADIUS = 648.0   # the figures' centres, on each tick's ray, upright
FIGURE_SIZE = 58.0
LEGEND_Y = 594.0        # the legend's centre (IN or OUT)
LEGEND_SIZE = 80.0


def position(db: float) -> float:
    """0..1 along the sweep. Half-power law: sqrt(10^(dB/20))."""
    lo, hi = 10 ** (DB_MIN / 40), 10 ** (DB_MAX / 40)
    return (10 ** (db / 40) - lo) / (hi - lo)


def angle(db: float) -> float:
    """SVG rotate() angle: -SWEEP at DB_MIN (left) ... +SWEEP at DB_MAX (right)."""
    return -SWEEP + 2 * SWEEP * position(db)


def polar(r: float, deg: float):
    a = math.radians(deg)
    return PIVOT[0] + r * math.sin(a), PIVOT[1] - r * math.cos(a)


# ----------------------------------------------------------------- shared defs
DEFS = """<defs>
<linearGradient id="bezel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3a3c"/><stop offset="0.5" stop-color="#1c1c1e"/><stop offset="1" stop-color="#0a0a0b"/></linearGradient>
<linearGradient id="bezelEdge" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#6b6b6e"/><stop offset="1" stop-color="#2a2a2c"/></linearGradient>
<radialGradient id="backlight" cx="500" cy="720" r="620" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#ffd48f"/><stop offset="0.35" stop-color="#f7b96a"/><stop offset="0.7" stop-color="#dd9243"/><stop offset="1" stop-color="#a86224"/></radialGradient>
<radialGradient id="vignette" cx="500" cy="480" r="520" gradientUnits="userSpaceOnUse"><stop offset="0.55" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="0.55"/></radialGradient>
<linearGradient id="glass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.22"/><stop offset="0.45" stop-color="#fff" stop-opacity="0.04"/><stop offset="0.5" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="innerShade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity="0.45"/><stop offset="0.12" stop-color="#000" stop-opacity="0"/><stop offset="0.9" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="0.35"/></linearGradient>
<clipPath id="window"><rect x="92" y="192" width="816" height="556" rx="28"/></clipPath>
<filter id="glow" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="18"/></filter>
</defs>"""

HEAD = '<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000" viewBox="0 0 1000 1000">\n' + DEFS + "\n"


def face() -> str:
    o = [HEAD]
    o.append("<!-- housing -->")
    o.append('<rect x="40" y="140" width="920" height="720" rx="56" fill="url(#bezelEdge)"/>')
    o.append('<rect x="46" y="146" width="908" height="708" rx="52" fill="url(#bezel)"/>')
    o.append('<rect x="92" y="192" width="816" height="556" rx="28" fill="#ff9a3a" opacity="0.35" filter="url(#glow)"/>')
    o.append('<rect x="84" y="184" width="832" height="572" rx="32" fill="#050505"/>')
    o.append("<!-- backlit face -->")
    o.append('<g clip-path="url(#window)">')
    for grad in ("backlight", "vignette", "innerShade"):
        o.append(f'<rect x="92" y="192" width="816" height="556" fill="url(#{grad})"/>')

    # main arc: black up to 0 dB, red from 0 dB to +6
    a0, aZ, a1 = angle(DB_MIN), angle(0.0), angle(DB_MAX)
    x0, y0 = polar(ARC_R, a0)
    xz, yz = polar(ARC_R, aZ)
    x1, y1 = polar(ARC_R, a1)
    o.append(f'<path d="M {x0:.1f} {y0:.1f} A {ARC_R} {ARC_R} 0 0 1 {xz:.1f} {yz:.1f}" fill="none" stroke="{INK}" stroke-width="10" stroke-linecap="round"/>')
    o.append(f'<path d="M {xz:.1f} {yz:.1f} A {ARC_R} {ARC_R} 0 0 1 {x1:.1f} {y1:.1f}" fill="none" stroke="{RED}" stroke-width="16" stroke-linecap="butt"/>')

    # ticks: inner end sits on the arc (y = 880-558 = 322), majors 50 long, minors 28
    o.append(f'<g stroke="{INK}" stroke-width="9" stroke-linecap="round">')
    for db in MAJOR:
        col = RED if db > 0 else INK
        o.append(f'<g transform="rotate({angle(db):.2f} 500 880)"><line x1="500" y1="322" x2="500" y2="272" stroke="{col}"/></g>')
    o.append("</g>")
    o.append(f'<g stroke="{INK}" stroke-width="5">')
    for db in MINOR:
        col = RED if db > 0 else INK
        o.append(f'<g transform="rotate({angle(db):.2f} 500 880)"><line x1="500" y1="322" x2="500" y2="294" stroke="{col}"/></g>')
    o.append("</g>")

    # The figures and the legend are the plugin's (see the module docstring).
    o.append("</g>")
    o.append("</svg>")
    return "\n".join(o) + "\n"


def needle() -> str:
    return HEAD + f"""<!-- NEEDLE: pivot (500,880). angle -{SWEEP:.0f} = {DB_MIN:.0f} dB (left), +{SWEEP:.0f} = +{DB_MAX:.0f} dB (right). Rotate only this group. -->
<g id="needle" transform="rotate(0 500 880)">
<polygon points="490,880 510,880 502.5,260 500,236 497.5,260" fill="#111"/>
<polygon points="498.6,300 501.4,300 500.8,262 500,250 499.2,262" fill="{RED}"/>
</g>
</svg>
"""


def glass() -> str:
    return HEAD + """<!-- glass + bezel detail: draw ABOVE the needle -->
<g clip-path="url(#window)">
<rect x="92" y="192" width="816" height="556" fill="url(#glass)"/>
<path d="M 92 192 L 460 192 L 240 748 L 92 748 Z" fill="#fff" opacity="0.06"/>
</g>
<rect x="92" y="192" width="816" height="556" rx="28" fill="none" stroke="#000" stroke-opacity="0.6" stroke-width="4"/>
<g fill="#8a8a8e" stroke="#222" stroke-width="2"><circle cx="78" cy="178" r="11"/><circle cx="922" cy="178" r="11"/><circle cx="78" cy="822" r="11"/><circle cx="922" cy="822" r="11"/></g>
</svg>
"""


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    args = parser.parse_args()
    for name, fn in (("level_meter_face.svg", face), ("level_meter_needle.svg", needle), ("level_meter_glass.svg", glass)):
        p = os.path.join(args.out, name)
        with open(p, "w") as fh:
            fh.write(fn())
        print("wrote", p)
    print("tick angles:", {db: round(angle(db), 2) for db in MAJOR})
    print("figures at radius", FIGURE_RADIUS, {db: (round(x, 1), round(y, 1)) for db in FIGURES for x, y in [polar(FIGURE_RADIUS, angle(db))]})

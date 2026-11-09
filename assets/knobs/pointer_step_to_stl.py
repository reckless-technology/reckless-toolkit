"""Tessellate a knob STEP's largest solid to one binary STL, as finely as asked.

    ~/cad-venv/bin/python3 pointer_step_to_stl.py pointer_knob.step pointer_knob.stl 0.015 0.10

Where knob_step_to_stl.py splits the chicken head into two meshes - its body and its index line, which
are separate faces of one solid and take separate materials - this writes the solid whole, because the
pointer knob's indicator is found by shape at render time instead (render_pointer_knob.py, --mark-*).
One mesh also keeps the knurled ring's faces together, which a split would have to divide arbitrarily.

The tolerances are the linear and angular ones OCCT tessellates to, in millimetres and radians. They
default far finer than knob_step_to_stl.py's 0.02 / 0.12, which suit the chicken head's smooth shell:
the pointer knob carries sixteen flutes and a knurled ring whose facets show at 0.02. The committed
strip was made at 0.015 / 0.10, which gives 276,860 triangles - 0.005 / 0.05 gives 1,351,538 and is
indistinguishable at 256 px, so it is not worth the render time.

cadquery lives in a venv of its own on this desktop (`python3 -m venv ~/cad-venv &&
~/cad-venv/bin/pip install cadquery`); under WSL it reads the Windows file through /mnt/c.
"""
import sys

import cadquery as cq

src, dst = sys.argv[1], sys.argv[2]
lin = float(sys.argv[3]) if len(sys.argv) > 3 else 0.015
ang = float(sys.argv[4]) if len(sys.argv) > 4 else 0.10

shape = cq.importers.importStep(src)
solids = shape.solids().vals()
knob = max(solids, key=lambda s: s.Volume())
bb = knob.BoundingBox()
print(f"solid: volume {knob.Volume():.2f} mm^3, {len(knob.Faces())} faces, {len(solids)} in the file")
print(f"  x[{bb.xmin:.3f},{bb.xmax:.3f}] y[{bb.ymin:.3f},{bb.ymax:.3f}] z[{bb.zmin:.3f},{bb.zmax:.3f}]")
knob.exportStl(dst, tolerance=lin, angularTolerance=ang, ascii=False)
print(f"wrote {dst} at tolerance {lin} / {ang}")

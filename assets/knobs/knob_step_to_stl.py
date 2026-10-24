"""Turn the chicken-head knob's STEP into the two meshes render_knob.py wants: the body and the index line.

    ~/cad-venv/bin/python3 knob_step_to_stl.py chicken_head_3.step ch3 1.5   ->  ch3_body.stl, ch3_mark.stl

The STEP is chicken_head_3.step beside this script, SolidWorks' AP214 export of the part chicken_head_3.sldprt (the
model to open and change; export a new STEP from it). It is drawn in inches, which cadquery's import converts to
millimetres, the unit everything here is in. It holds two solids - the knob and the brass insert inside it - with
the shaft along +Y, the base at y = 0 and the nose along +Z; the skirt is 7.94 mm in radius (5/16 in) and the nose
reaches 19.05 mm (3/4 in). The index line is not a separate solid but two faces of the knob's, a strip
1.36 mm wide straddling x = 0 down the nose's leading edge; the STEP colours them differently, but that colour
does not survive the OCCT import, so they are found by shape: the faces narrower than the third argument in x
that straddle x = 0 on the nose's half. Everything else is body. Both come out as binary STL (cadquery venv:
`python3 -m venv ~/cad-venv && ~/cad-venv/bin/pip install cadquery`).
"""
import sys
import cadquery as cq

src, out, xmax = sys.argv[1], sys.argv[2], float(sys.argv[3])
shape = cq.importers.importStep(src)
knob = max(shape.solids().vals(), key=lambda s: s.Volume())
faces = knob.Faces()
print("faces", len(faces))
mark, body = [], []
for f in faces:
    bb = f.BoundingBox()
    narrow = bb.xmin < 0.0 < bb.xmax and (bb.xmax - bb.xmin) <= xmax and bb.zmin > 5.0
    if narrow:
        print(f"  mark? area {f.Area():7.2f}  x[{bb.xmin:6.2f},{bb.xmax:6.2f}] y[{bb.ymin:6.2f},{bb.ymax:6.2f}] z[{bb.zmin:6.2f},{bb.zmax:6.2f}] {f.geomType()}")
    (mark if narrow else body).append(f)
for name, fs in (("mark", mark), ("body", body)):
    comp = cq.Compound.makeCompound(fs)
    comp.exportStl(f"{out}_{name}.stl", tolerance=0.02, angularTolerance=0.12, ascii=False)
    print("wrote", f"{out}_{name}.stl", len(fs), "faces")

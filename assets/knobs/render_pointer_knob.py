"""Render the Polysynth's pointer knob from above, one frame per angle, in render_knob.py's scene.

RecklessPointer.png beside this script is 128 frames of 256 px, and these three commands remake it
exactly - the defaults are this knob's, so nothing but the paths needs saying:

    ~/cad-venv/bin/python3 pointer_step_to_stl.py pointer_knob.step /tmp/pointer_knob.stl
    blender -b -P render_pointer_knob.py -- --stl /tmp/pointer_knob.stl --out /tmp/frames
    python3 assemble_strip.py /tmp/frames RecklessPointer.png --quantize

Give Blender absolute paths; a relative --out has lost a run's frames before. --only 0 --size 512
renders a single frame to look at, and --show-mark renders the material selection in false colour -
magenta the indicator, green the base disc - which is worth doing before 128 frames are paid for.

The difference from render_knob.py is the mesh and the finish, not the light, which is shared exactly
so that the two knobs sit on a panel together. The chicken head arrives as two solids from its STEP,
body and index line, and each takes a material whole. This knob is one solid whose indicator is a tab
standing proud of the top face, so the cream is cut out of the one mesh by shape: faces inside the box
--mark-width by --mark-from by --mark-floor take it. --mark-raised says the tab stands above the top
face rather than being cut into it, which decides whether the knob's highest plane is the mark's or
the body's - it is the mark's here, and getting that backwards silently loses the most visible part
of the indicator. A third material, the base disc below --base-top, is off for this knob (0.0): its
flutes and skirt give it relief enough, where a flat-topped knob needs the two tones to read at all.

The mesh is expected shaft-up: the knob's axis along +Z, the base at z = 0 and the indicator toward
+Y, which is how pointer_knob.step exports. It is recentred on its own bounding box, so an offset does
not matter, and --spin turns the mesh - before the materials are picked out, so the indicator test
still looks toward +Y - if a model points elsewhere.

Everything the look layer depends on is render_knob.py's and must stay so: square frames, the shaft at
the frame's centre, the indicator at twelve in frame 0 and clockwise by 2 pi i / frames in frame i, and
the half-width `margin` (1.4) reaches. Change --margin and change look::kKnobMargin (look/LookMetrics.h)
with it; the frame count the look layer reads from the strip's height, so it needs no telling.
--size-scale draws the knob smaller inside that unchanged frame (0.8 here), which is how this knob is
sized against the chicken head without touching the margin every other plugin's knob is pinned to.
"""
import argparse
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
p = argparse.ArgumentParser()
p.add_argument("--stl", required=True)
p.add_argument("--out", required=True)
p.add_argument("--frames", type=int, default=128)
p.add_argument("--size", type=int, default=256)
p.add_argument("--reach", type=float, default=0.0, help="mm from the shaft to the rim; 0 = measure it")
p.add_argument("--margin", type=float, default=1.4, help="half-width of the frame in reaches")
p.add_argument("--samples", type=int, default=128)
p.add_argument("--only", type=int, default=-1, help="render this frame alone")
p.add_argument("--sun", type=float, default=2.6, help="key light strength")
p.add_argument("--fill", type=float, default=0.11, help="world fill strength")
p.add_argument("--spin", type=float, default=0.0, help="degrees about Z to bring the indicator to twelve")
p.add_argument("--smooth", type=float, default=30.0, help="shade smooth below this angle, in degrees")
# The indicator, in millimetres, in the recentred mesh. Two kinds of knob carry one: a slot cut into
# the top face (engraved, the default) and a tab standing proud of it (--mark-raised). The test is
# the same box either way; what differs is the top face itself, which an engraved mark must exclude
# - it is the body's - and a raised mark must keep, since the tab's own top is the highest face there is.
p.add_argument("--mark-width", type=float, default=5.0, help="full width of the mark in x")
p.add_argument("--mark-floor", type=float, default=15.52, help="lowest z a mark face may have")
p.add_argument("--mark-from", type=float, default=-2.0, help="lowest y a mark face may have")
p.add_argument("--mark-raised", action=argparse.BooleanOptionalAction, default=True,
               help="the mark stands proud of the top face rather than being cut into it")
p.add_argument("--show-mark", action="store_true", help="render the material selection in false colour")
# the knob's drawn size within the frame, independent of the look layer's geometry: 0.8 draws it a
# fifth smaller without moving --margin, which look::kKnobMargin is pinned to.
p.add_argument("--size-scale", type=float, default=0.8, help="the knob's drawn size, 1.0 = rim to margin")
# The finish: a charcoal body and a cream indicator. The body can take a second, darker tone below
# --base-top, for a knob whose step would otherwise be unreadable under a single flat grey; this one
# does not need it (0.0 leaves nothing below the line) because its flutes and skirt carry the relief.
p.add_argument("--body", default="0.035,0.035,0.038", help="body base colour, linear rgb")
p.add_argument("--base", default="0.012,0.012,0.014", help="base disc colour, linear rgb")
p.add_argument("--base-top", type=float, default=0.0, help="mm: faces below this take the darker tone")
p.add_argument("--mark", default="0.86,0.80,0.66", help="indicator base colour, linear rgb")
p.add_argument("--body-rough", type=float, default=0.30)
p.add_argument("--body-coat", type=float, default=0.15)
a = p.parse_args(argv)
os.makedirs(a.out, exist_ok=True)


def rgb(s):
    return tuple(float(x) for x in s.split(","))


bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
prefs = bpy.context.preferences.addons["cycles"].preferences
prefs.compute_device_type = "OPTIX"
prefs.refresh_devices()
gpu = False
for d in prefs.devices:
    d.use = d.type == "OPTIX"
    gpu = gpu or d.use
scene.cycles.device = "GPU" if gpu else "CPU"
scene.cycles.samples = a.samples
scene.cycles.use_denoising = True
scene.render.film_transparent = True
scene.render.resolution_x = scene.render.resolution_y = a.size
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.render.image_settings.color_depth = "8"
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"

# World: a grey fill, so the dark plastic has something to reflect and its unlit side is not void.
world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.7, 0.7, 0.72, 1.0)
bg.inputs["Strength"].default_value = a.fill

# Key: render_knob.py's sun, from the upper left of the image, softened to 14 degrees.
sun_data = bpy.data.lights.new("Key", type="SUN")
sun_data.energy = a.sun
sun_data.angle = math.radians(14.0)
sun = bpy.data.objects.new("Key", sun_data)
scene.collection.objects.link(sun)
sun.rotation_euler = Vector((1.0, -1.0, -3.9)).normalized().to_track_quat("-Z", "Y").to_euler()

cam_data = bpy.data.cameras.new("Camera")
cam_data.type = "ORTHO"
cam_data.clip_end = 1000.0
cam = bpy.data.objects.new("Camera", cam_data)
cam.location = (0.0, 0.0, 200.0)
cam.rotation_euler = (0.0, 0.0, 0.0)
scene.collection.objects.link(cam)
scene.camera = cam

bpy.ops.mesh.primitive_plane_add(size=1000.0, location=(0.0, 0.0, 0.0))
bpy.context.active_object.is_shadow_catcher = True


def material(name, base, roughness, coat, coat_roughness=0.08):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    n = m.node_tree.nodes["Principled BSDF"]
    n.inputs["Base Color"].default_value = (*base, 1.0)
    n.inputs["Roughness"].default_value = roughness
    n.inputs["Coat Weight"].default_value = coat
    n.inputs["Coat Roughness"].default_value = coat_roughness
    return m


# --- the mesh, recentred on its own axis with its base on the panel
bpy.ops.wm.stl_import(filepath=a.stl)
obj = bpy.context.selected_objects[0]
obj.name = "Knob"
mesh = obj.data

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
shift = Vector((-(min(xs) + max(xs)) / 2.0, -(min(ys) + max(ys)) / 2.0, -min(zs)))
for v in mesh.vertices:
    v.co += shift

# --spin turns the mesh itself, not the object, so that it happens before the slot is picked out
# below: the predicate always looks for an indicator toward +Y, and a model exported pointing
# elsewhere is brought round to meet it rather than the predicate being rewritten per model.
if a.spin:
    turn = math.radians(a.spin)
    c_t, s_t = math.cos(turn), math.sin(turn)
    for v in mesh.vertices:
        v.co.x, v.co.y = v.co.x * c_t - v.co.y * s_t, v.co.x * s_t + v.co.y * c_t

top = max(v.co.z for v in mesh.vertices)
radius = max(math.hypot(v.co.x, v.co.y) for v in mesh.vertices)
reach = a.reach if a.reach > 0.0 else radius
cam_data.ortho_scale = 2.0 * a.margin * reach / a.size_scale
print(f"[knob] height {top:.3f} mm, outer radius {radius:.3f} mm, reach {reach:.3f} mm, "
      f"frame {cam_data.ortho_scale:.3f} mm", flush=True)

# --- three slots of material: the raised section, the base disc under it, and the indicator.
body_mat = material("Body", rgb(a.body), a.body_rough, a.body_coat)
mark_mat = material("Mark", (1.0, 0.0, 1.0) if a.show_mark else rgb(a.mark), 0.45, 0.0)
base_mat = material("Base", (0.0, 1.0, 0.0) if a.show_mark else rgb(a.base), a.body_rough, a.body_coat)
mesh.materials.append(body_mat)
mesh.materials.append(mark_mat)
mesh.materials.append(base_mat)

# A face belongs to the indicator when it lies inside the slot cut into the top: narrow in x,
# forward in y, high in z, and not flush with the top face itself (which is the body's). The
# shaft bore runs up the middle and is narrow in x too, so the floor test is what separates them -
# the bore is open well below the slot's floor.
half = a.mark_width / 2.0
marked = 0
for poly in mesh.polygons:
    vs = [mesh.vertices[i].co for i in poly.vertices]
    c = sum(vs, Vector((0.0, 0.0, 0.0))) / len(vs)
    # a face lying wholly in the knob's highest plane: the body's own top for an engraved mark,
    # but the tab's top - the mark itself - for a raised one
    at_top = min(v.z for v in vs) > top - 1e-3
    inside = (abs(c.x) <= half and c.y >= a.mark_from and c.z >= a.mark_floor
              and (a.mark_raised or not at_top))
    if inside:
        poly.material_index = 1
        marked += 1
    else:
        # the base disc is everything whose face lies wholly below the step; a face straddling it
        # takes the raised section's tone, so the step itself stays a clean edge
        poly.material_index = 2 if max(v.z for v in vs) <= a.base_top else 0
print(f"[knob] indicator faces: {marked} of {len(mesh.polygons)}  "
      f"(|x| <= {half:.2f}, y >= {a.mark_from:.2f}, z >= {a.mark_floor:.2f}, "
      f"{'raised' if a.mark_raised else 'engraved'})", flush=True)
print(f"[knob] base-disc faces: {sum(1 for p in mesh.polygons if p.material_index == 2)} "
      f"(wholly below z = {a.base_top:.2f})", flush=True)
if marked == 0:
    print("[knob] WARNING: no face matched the slot - the indicator will not show", flush=True)

# Smooth the turned surfaces, keep the slot's and the top face's edges sharp.
bpy.context.view_layer.objects.active = obj
bpy.ops.object.shade_smooth_by_angle(angle=math.radians(a.smooth))

rig = bpy.data.objects.new("Rig", None)
scene.collection.objects.link(rig)
obj.parent = rig

frames = range(a.frames) if a.only < 0 else [a.only]
for i in frames:
    theta = 2.0 * math.pi * i / a.frames  # clockwise from twelve, the slider's angle
    rig.rotation_euler = (0.0, 0.0, -theta)
    scene.render.filepath = os.path.join(a.out, f"frame_{i:03d}.png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath, flush=True)

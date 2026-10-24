"""Render a chicken-head knob from above, one frame per pointer angle, lit by a fixed light so shading and shadow
belong to the panel, not the knob.

    blender -b -P render_knob.py -- --body ch3_body.stl --mark ch3_mark.stl --out frames --frames 128 --size 256
                                    [--only 0 --size 512]   a single frame for inspection

The meshes come from the STEP in millimetres with the shaft along +Y, the base at y = 0 and the nose along +Z;
they are stood up (Y -> Z) and turned so the nose points to the top of the image at angle 0, then clockwise by
2 pi i / frames for frame i - the slider's convention. The camera is orthographic straight down, framing
2 * margin * reach millimetres, so the nose sweeps a circle of 1 / margin of the half-width and the shadow has
room beyond it. Key light: a sun from the upper left, softened; fill: a grey world. Transparent film with a
shadow catcher, so the frame carries the knob and its shadow over nothing else.

RecklessChickenHead.png is 128 such frames of 256 px, stacked by assemble_strip.py (--quantize: a 256-colour
palette, which no eye tells from the truecolour strip at knob size and is an eleventh of its bytes), from the meshes
knob_step_to_stl.py makes of chicken_head_3.step (beside this script). Rendered with a portable Blender 4.2 on the GPU (OptiX), about
a second a frame:

    blender -b -P render_knob.py -- --body ch3_body.stl --mark ch3_mark.stl --out frames --frames 128 --size 256
    python3 assemble_strip.py frames RecklessChickenHead.png --quantize

The series' look (look) assumes what this script fixes: square frames, the shaft at the frame's centre, the
nose at twelve in frame 0 and clockwise by 2 pi i / frames in frame i, the half-width `margin` (1.4) nose reaches.
Change --margin and change look::kKnobMargin (look/LookMetrics.h) with it; the frame count it reads from the
strip.
"""
import argparse
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
p = argparse.ArgumentParser()
p.add_argument("--body", required=True)
p.add_argument("--mark", required=True)
p.add_argument("--out", required=True)
p.add_argument("--frames", type=int, default=128)
p.add_argument("--size", type=int, default=256)
p.add_argument("--reach", type=float, default=19.05, help="mm from the shaft to the nose")
p.add_argument("--margin", type=float, default=1.4, help="half-width of the frame in reaches")
p.add_argument("--samples", type=int, default=128)
p.add_argument("--only", type=int, default=-1, help="render this frame alone")
p.add_argument("--sun", type=float, default=2.6, help="key light strength")
p.add_argument("--fill", type=float, default=0.11, help="world fill strength")
a = p.parse_args(argv)
os.makedirs(a.out, exist_ok=True)

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

# World: a grey fill so the black plastic has something to reflect and its unlit side is not void.
world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.7, 0.7, 0.72, 1.0)
bg.inputs["Strength"].default_value = a.fill

# Key: a sun from the upper left of the image, high above the panel, softened to 14 degrees.
sun_data = bpy.data.lights.new("Key", type="SUN")
sun_data.energy = a.sun
sun_data.angle = math.radians(14.0)
sun = bpy.data.objects.new("Key", sun_data)
scene.collection.objects.link(sun)
towards = Vector((1.0, -1.0, -3.9)).normalized()  # from the light, down onto the knob: 70 degrees up, so the
# 19 mm back throws 7 mm of shadow and stays inside the slider's cell at every angle of the sweep
sun.rotation_euler = towards.to_track_quat("-Z", "Y").to_euler()

# Camera: orthographic, straight down, +Y up.
cam_data = bpy.data.cameras.new("Camera")
cam_data.type = "ORTHO"
cam_data.ortho_scale = 2.0 * a.margin * a.reach
cam_data.clip_end = 1000.0
cam = bpy.data.objects.new("Camera", cam_data)
cam.location = (0.0, 0.0, 200.0)
cam.rotation_euler = (0.0, 0.0, 0.0)
scene.collection.objects.link(cam)
scene.camera = cam

# Shadow catcher: the panel, which shows nothing but the knob's shadow.
bpy.ops.mesh.primitive_plane_add(size=1000.0, location=(0.0, 0.0, 0.0))
plane = bpy.context.active_object
plane.is_shadow_catcher = True


def material(name, rgb, roughness, coat):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    n = m.node_tree.nodes["Principled BSDF"]
    n.inputs["Base Color"].default_value = (*rgb, 1.0)
    n.inputs["Roughness"].default_value = roughness
    n.inputs["Coat Weight"].default_value = coat
    n.inputs["Coat Roughness"].default_value = 0.08
    return m


bakelite = material("Bakelite", (0.016, 0.011, 0.008), 0.22, 0.3)
cream = material("Cream", (0.86, 0.80, 0.66), 0.45, 0.0)


def import_stl(path, name, mat):
    bpy.ops.wm.stl_import(filepath=path)
    obj = bpy.context.selected_objects[0]
    obj.name = name
    obj.data.materials.append(mat)
    for poly in obj.data.polygons:
        poly.use_smooth = True
    return obj


rig = bpy.data.objects.new("Rig", None)
scene.collection.objects.link(rig)
for path, name, mat in ((a.body, "Body", bakelite), (a.mark, "Mark", cream)):
    obj = import_stl(path, name, mat)
    obj.parent = rig
    # Stand the knob up: the STEP's Y (shaft) becomes Z; then turn its nose (+Z, now -Y) to +Y.
    obj.rotation_euler = (math.radians(90.0), 0.0, math.radians(180.0))

frames = range(a.frames) if a.only < 0 else [a.only]
for i in frames:
    theta = 2.0 * math.pi * i / a.frames  # clockwise from twelve, the slider's angle
    rig.rotation_euler = (0.0, 0.0, -theta)
    scene.render.filepath = os.path.join(a.out, f"frame_{i:03d}.png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath, flush=True)

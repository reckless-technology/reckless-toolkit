"""Stack rendered frames into one vertical film strip: frame i occupies rows [i * size, (i + 1) * size).

    python3 assemble_strip.py <frames dir> <out.png> [--quantize]

Reports the strip's size so the cost of frames x resolution can be judged before it goes into the binaries.
"""
import os
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
quantize = "--quantize" in sys.argv
names = sorted(n for n in os.listdir(src) if n.startswith("frame_") and n.endswith(".png"))
frames = [Image.open(os.path.join(src, n)).convert("RGBA") for n in names]
size = frames[0].width
assert all(f.size == (size, size) for f in frames)
strip = Image.new("RGBA", (size, size * len(frames)), (0, 0, 0, 0))
for i, f in enumerate(frames):
    strip.paste(f, (0, i * size))
if quantize:
    strip = strip.quantize(256, method=Image.FASTOCTREE)
strip.save(dst, optimize=True)
print(f"{len(frames)} frames of {size} px -> {dst}: {os.path.getsize(dst) / 1e6:.2f} MB")

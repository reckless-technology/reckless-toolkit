"""The series' app icon in the form a platform's executable carries it, from the 1024 px render
(assets/logos/RecklessAppIcon.png, made by make_app_icon.py there): a macOS bundle's .icns, every size the
format holds, or a Windows executable's .ico, the sizes Explorer and the task bar ask for, each a PNG inside it.
The bundles' recipes run it (cmake/RecklessBundle.cmake's reckless_bundle_icon), for the Standalones and the VST3s.

    python make_app_icons.py <icon.png> <out.icns | out.ico>
"""

import sys

from PIL import Image

ICO_SIZES = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]


def main(source, target):
    icon = Image.open(source).convert("RGBA")
    if target.lower().endswith(".icns"):
        icon.save(target, format="ICNS")
    elif target.lower().endswith(".ico"):
        icon.save(target, format="ICO", sizes=ICO_SIZES)
    else:
        raise SystemExit(f"make_app_icons.py: {target} is neither .icns nor .ico")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])

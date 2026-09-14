#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — regenerate the per-platform icon files from the master (issue #103).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# The three launchers each want the one icon in their own container: the macOS bundle a .icns,
# the Windows executable and installer a .ico, the Linux desktop entry a hicolor PNG per size.
# All of them are GENERATED FROM naudio.png HERE AND CHECKED IN — not produced by the build —
# because the build hosts do not share a tool that can write all three (iconutil exists only on
# macOS; nothing on a Windows or Linux runner writes a .icns), and an icon is not something the
# packaging should depend on a host's image toolchain for. Run this after replacing naudio.png,
# commit what it writes, and nothing else changes.
#
#   python3 packaging/icons/regenerate.py [path/to/master.png]
#
# Needs Pillow (`pip install Pillow`) and, for the .icns, macOS's iconutil. The master is the
# operator's artwork resized to 1024×1024 — the largest size any consumer reads (the .icns's
# 512×512@2x) — so a larger source is reduced once, here, and the checked-in master is what every
# derivative is made from. Sizes are written with Lanczos resampling; the .ico carries BMP-format
# entries rather than PNG-compressed ones because every Windows since XP and every NSIS reads
# those, and the difference is ~300 KB in a file nobody downloads on its own.
import os
import shutil
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    sys.exit("regenerate.py: needs Pillow (pip install Pillow)")

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "naudio.png")
MASTER_SIZE = 1024
ICNS_SIZES = (16, 32, 128, 256, 512)                # each also at @2x, per Apple's iconset
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)          # Explorer, the taskbar, Add/Remove, NSIS
HICOLOR_SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512)  # the hicolor theme's standard set


def resized(img, size):
    return img.resize((size, size), Image.LANCZOS)


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else MASTER
    img = Image.open(source).convert("RGBA")
    if img.width != img.height:
        sys.exit(f"regenerate.py: {source} is {img.width}×{img.height}; the icon must be square")
    if img.width < MASTER_SIZE:
        sys.exit(f"regenerate.py: {source} is {img.width}px; the master is {MASTER_SIZE}px")
    if img.width != MASTER_SIZE:
        img = resized(img, MASTER_SIZE)
    img.save(MASTER, optimize=True)
    print(f"wrote {MASTER} ({MASTER_SIZE}×{MASTER_SIZE})")

    # macOS: an .iconset directory packed by iconutil. The @2x member of a size is the next
    # size's pixels under the retina name; iconutil refuses an iconset with a missing member.
    iconutil = shutil.which("iconutil")
    if iconutil:
        with tempfile.TemporaryDirectory() as tmp:
            iconset = os.path.join(tmp, "naudio.iconset")
            os.mkdir(iconset)
            for s in ICNS_SIZES:
                resized(img, s).save(os.path.join(iconset, f"icon_{s}x{s}.png"))
                resized(img, 2 * s).save(os.path.join(iconset, f"icon_{s}x{s}@2x.png"))
            icns = os.path.join(HERE, "naudio.icns")
            subprocess.run([iconutil, "-c", "icns", iconset, "-o", icns], check=True)
            print(f"wrote {icns}")
    else:
        print("no iconutil on this host: naudio.icns NOT regenerated (run this on macOS)")

    # Windows: one .ico holding every size Explorer, the taskbar and NSIS pick from.
    ico = os.path.join(HERE, "naudio.ico")
    img.save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES], bitmap_format="bmp")
    print(f"wrote {ico}")

    # Linux: the hicolor layout the desktop entry's Icon=naudio resolves through.
    for s in HICOLOR_SIZES:
        d = os.path.join(HERE, "hicolor", f"{s}x{s}", "apps")
        os.makedirs(d, exist_ok=True)
        resized(img, s).save(os.path.join(d, "naudio.png"), optimize=True)
    print(f"wrote hicolor/{{{','.join(str(s) for s in HICOLOR_SIZES)}}}x*/apps/naudio.png")


if __name__ == "__main__":
    main()

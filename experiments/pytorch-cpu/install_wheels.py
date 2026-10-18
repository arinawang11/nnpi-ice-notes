#!/usr/bin/env python3
"""Extract .whl files into a site-packages directory without pip.

Usage: install_wheels.py <site_dir> <wheel_dir> [<wheel_dir> ...]
Handles wheel .data/purelib and .data/platlib redirection.
"""
import glob
import os
import shutil
import sys
import zipfile

site = sys.argv[1]
os.makedirs(site, exist_ok=True)

for wdir in sys.argv[2:]:
    for whl in sorted(glob.glob(os.path.join(wdir, "*.whl"))):
        with zipfile.ZipFile(whl) as zf:
            zf.extractall(site)
        print("extracted", os.path.basename(whl))

for data in glob.glob(os.path.join(site, "*.data")):
    for sub in ("purelib", "platlib"):
        src = os.path.join(data, sub)
        if os.path.isdir(src):
            for item in os.listdir(src):
                shutil.move(os.path.join(src, item), os.path.join(site, item))
    shutil.rmtree(data)

print("site ready:", site)

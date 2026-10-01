#!/usr/bin/env python3
"""Pad the assembled boot image out to a whole number of 512-byte sectors.

The loader in boot.asm reads a fixed count of sectors, so the file has to be
exactly that long: a short image would leave whatever the previous write put
in the image file behind as stage 2.
"""

import sys

path = sys.argv[1]
sectors = int(sys.argv[2])
want = sectors * 512

with open(path, "rb") as fh:
    data = fh.read()

if len(data) > want:
    sys.exit(f"pad: {path} is {len(data)} bytes, which does not fit in "
             f"{sectors} sectors ({want} bytes) -- raise STAGE2_SECTORS")

print(f"pad: {len(data)} -> {want} bytes ({sectors} sectors)")
with open(path, "wb") as fh:
    fh.write(data + b"\0" * (want - len(data)))

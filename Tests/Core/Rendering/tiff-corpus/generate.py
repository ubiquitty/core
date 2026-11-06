#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Hand-construct minimal baseline uncompressed TIFFs that TIFF_Decode accepts:
# 8 bps, SamplesPerPixel 3 (RGB) or 4 (RGBA), Photometric 2 (RGB),
# Compression 1, PlanarConfig 1, single strip. Layout:
#   header(8) | IFD(2 + N*12 + 4) | BitsPerSample array | pixel strip
import struct, sys, os

T_SHORT, T_LONG = 3, 4

def build(le, w, h, spp):
    bo = '<' if le else '>'
    mark = b'II' if le else b'MM'
    bps_count = spp  # one SHORT per sample
    bps_bytes = bps_count * 2

    # Tags (sorted ascending, as baseline requires).
    # (tag, type, count, value_or_offset). Offsets are patched after we know
    # where the BitsPerSample array and strip live.
    entries = [
        (256, T_SHORT, 1, w),          # ImageWidth
        (257, T_SHORT, 1, h),          # ImageLength
        (258, T_SHORT, spp, None),     # BitsPerSample -> array offset
        (259, T_SHORT, 1, 1),          # Compression = none
        (262, T_SHORT, 1, 2),          # Photometric = RGB
        (273, T_LONG,  1, None),       # StripOffsets -> strip offset
        (277, T_SHORT, 1, spp),        # SamplesPerPixel
        (278, T_SHORT, 1, h),          # RowsPerStrip = full image (1 strip)
        (279, T_LONG,  1, w * h * spp),# StripByteCounts
        (284, T_SHORT, 1, 1),          # PlanarConfig = chunky
    ]
    n = len(entries)
    ifd_off = 8
    ifd_len = 2 + n * 12 + 4
    bps_off = ifd_off + ifd_len
    strip_off = bps_off + bps_bytes

    out = bytearray()
    out += mark
    out += struct.pack(bo + 'H', 42)
    out += struct.pack(bo + 'I', ifd_off)
    out += struct.pack(bo + 'H', n)
    for tag, typ, count, val in entries:
        if tag == 258:
            val = bps_off
        elif tag == 273:
            val = strip_off
        # Pack the value field. SHORT count==1 sits left-justified in the
        # 4-byte field (already how read16(p+8) reads it); LONG fills it.
        if typ == T_SHORT and count == 1:
            field = struct.pack(bo + 'H', val) + b'\x00\x00' if le else struct.pack(bo + 'H', val) + b'\x00\x00'
            # For big-endian, a SHORT also occupies the first 2 bytes of the field.
            field = (struct.pack(bo + 'H', val) + b'\x00\x00') if le else (struct.pack(bo + 'H', val) + b'\x00\x00')
        else:
            field = struct.pack(bo + 'I', val)
        out += struct.pack(bo + 'HHI', tag, typ, count) + field
    out += struct.pack(bo + 'I', 0)  # next IFD = none
    # BitsPerSample array: all 8.
    for _ in range(spp):
        out += struct.pack(bo + 'H', 8)
    assert len(out) == strip_off, (len(out), strip_off)
    # Pixel data: a deterministic gradient.
    for y in range(h):
        for x in range(w):
            px = [(x * 37) & 0xFF, (y * 53) & 0xFF, ((x + y) * 17) & 0xFF, 0xFF]
            out += bytes(px[:spp])
    return bytes(out)

def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    cases = [
        ('rgb_le_4x3.tiff',  build(True,  4, 3, 3)),
        ('rgba_le_2x2.tiff', build(True,  2, 2, 4)),
        ('rgb_be_3x2.tiff',  build(False, 3, 2, 3)),
    ]
    for name, blob in cases:
        with open(os.path.join(d, name), 'wb') as f:
            f.write(blob)
        print(name, len(blob), 'bytes')

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Generate a minimal Windows .res file containing an RT_GROUP_ICON resource
(numeric ID 1) plus RT_ICON entries, from a PNG source.

The .res file can be linked directly into an MSVC executable with link.exe.
Windows Explorer uses resource ID 1 as the application icon.

Usage:
    python gen_win_resources.py <input.png> <output.res>
"""

import struct
import sys
from PIL import Image

RT_ICON = 3
RT_GROUP_ICON = 14


def _make_dib(img):
    """Convert a PIL RGBA image to a Windows DIB (XOR mask + AND mask).

    Returns raw bytes suitable for an RT_ICON resource.
    """
    img = img.convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())

    bi_size = 40
    bi_height = h * 2  # doubled: XOR mask + AND mask
    and_row = ((w + 31) // 32) * 4  # bytes per AND mask row, DWORD-aligned

    header = struct.pack(
        "<IiiHHIIiiII",
        bi_size,     # biSize
        w,           # biWidth
        bi_height,   # biHeight (doubled)
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression (BI_RGB)
        w * h * 4,   # biSizeImage
        0, 0,        # biXPelsPerMeter, biYPelsPerMeter
        0, 0,        # biClrUsed, biClrImportant
    )

    # XOR mask: BGRA, bottom-up row order
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            xor += bytes([b, g, r, a])

    # AND mask: 1 bit per pixel, 1 = transparent, bottom-up
    and_mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(and_row)
        for x in range(w):
            if pixels[y * w + x][3] < 128:
                row[x // 8] |= 1 << (7 - x % 8)
        and_mask += row

    return bytes(header) + bytes(xor) + bytes(and_mask)


def _res_header(data_size, type_id, name_id, language=0):
    """Build a 32-byte resource entry header for numeric type and name IDs."""
    h = struct.pack("<II", data_size, 32)
    h += struct.pack("<HH", 0xFFFF, type_id)
    h += struct.pack("<HH", 0xFFFF, name_id)
    h += struct.pack("<IHHII", 0, 0x0030, language, 0, 0)
    assert len(h) == 32, "header size mismatch"
    return h


def _pad4(data):
    """Pad bytes to the next DWORD boundary."""
    r = len(data) % 4
    return data + b"\x00" * (4 - r if r else 0)


def generate(src_png, out_res):
    img = Image.open(src_png).convert("RGBA")
    sizes = [16, 32, 48]  # standard sizes; 256-px PNG is added separately

    # Build DIB payloads for each size
    icons = []
    for size in sizes:
        resized = img.resize((size, size), Image.LANCZOS)
        dib = _make_dib(resized)
        icons.append(dib)

    buf = bytearray()

    # Null resource (32 zero bytes) required at start of .res files
    buf += b"\x00" * 32

    # Write RT_ICON entries (numeric IDs starting at 1)
    for idx, dib in enumerate(icons, start=1):
        buf += _res_header(len(dib), RT_ICON, idx)
        buf += _pad4(dib)

    # Write RT_GROUP_ICON entry (numeric ID 1 = Explorer application icon)
    # GRPICONDIR: WORD Reserved=0, WORD Type=1, WORD Count
    grp = struct.pack("<HHH", 0, 1, len(icons))
    for idx, dib in enumerate(icons, start=1):
        w = sizes[idx - 1]
        # GRPICONDIRENTRY: Width, Height, ColorCount, Reserved,
        #                  Planes, BitCount, BytesInRes, nId
        grp += struct.pack("<BBBBHHIH", w, w, 0, 0, 1, 32, len(dib), idx)
    buf += _res_header(len(grp), RT_GROUP_ICON, 1)
    buf += _pad4(grp)

    with open(out_res, "wb") as f:
        f.write(buf)

    print(
        f"gen_win_resources: wrote {out_res}  "
        f"({len(icons)} icon(s), {len(buf)} bytes total)"
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"Usage: {sys.argv[0]} <input.png> <output.res>")
    generate(sys.argv[1], sys.argv[2])

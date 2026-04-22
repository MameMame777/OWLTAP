#!/usr/bin/env python3
"""
Embed an application icon into a Windows .exe using the Windows
BeginUpdateResource / UpdateResource / EndUpdateResource API via ctypes.

This script is run once after `bazelisk build //src:jtag_viewer` to embed
the RT_GROUP_ICON + RT_ICON resources that make the .exe show the owl icon
in Windows Explorer and in the pinned taskbar entry.

Usage:
    python tools/embed_icon.py <exe_path> <png_path>

Example:
    python tools/embed_icon.py bazel-bin/src/jtag_viewer.exe docs/icon.png
"""

import ctypes
import ctypes.wintypes as wintypes
import struct
import sys
from PIL import Image

RT_ICON = 3
RT_GROUP_ICON = 14
LANG_NEUTRAL = 0


def _setup_api(k32):
    """Declare Win32 argtypes so integer resource IDs are passed as pointer
    values (equivalent to MAKEINTRESOURCE) rather than as 32-bit ints."""
    k32.BeginUpdateResourceW.argtypes = [ctypes.c_wchar_p, ctypes.c_bool]
    k32.BeginUpdateResourceW.restype = wintypes.HANDLE

    # lpType and lpName are LPCWSTR but accept MAKEINTRESOURCE(n) — an integer
    # cast to pointer.  Declaring them c_void_p lets Python ints pass through
    # as pointer-sized values, which is exactly what MAKEINTRESOURCE produces.
    k32.UpdateResourceW.argtypes = [
        wintypes.HANDLE,   # hUpdate
        ctypes.c_void_p,   # lpType  (MAKEINTRESOURCE or wide string ptr)
        ctypes.c_void_p,   # lpName  (MAKEINTRESOURCE or wide string ptr)
        wintypes.WORD,     # wLanguage
        ctypes.c_void_p,   # lpData
        wintypes.DWORD,    # cb
    ]
    k32.UpdateResourceW.restype = ctypes.c_bool

    k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_bool]
    k32.EndUpdateResourceW.restype = ctypes.c_bool


def _make_dib(img):
    """Convert a PIL RGBA image to a Windows DIB for RT_ICON."""
    img = img.convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())

    and_row = ((w + 31) // 32) * 4

    header = struct.pack(
        "<IiiHHIIiiII",
        40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0,
    )
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            xor += bytes([b, g, r, a])

    and_mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(and_row)
        for x in range(w):
            if pixels[y * w + x][3] < 128:
                row[x // 8] |= 1 << (7 - x % 8)
        and_mask += row

    return bytes(header) + bytes(xor) + bytes(and_mask)


def _update(k32, handle, type_id, name_id, data: bytes):
    """Call UpdateResourceW with integer MAKEINTRESOURCE IDs."""
    buf = (ctypes.c_char * len(data)).from_buffer_copy(data)
    ok = k32.UpdateResourceW(
        handle,
        type_id,    # integer → treated as MAKEINTRESOURCE(type_id)
        name_id,    # integer → treated as MAKEINTRESOURCE(name_id)
        LANG_NEUTRAL,
        buf,
        len(data),
    )
    if not ok:
        raise OSError(
            f"UpdateResource type={type_id} name={name_id} failed "
            f"(error {ctypes.GetLastError()})"
        )


def embed_icon(exe_path, png_path):
    img = Image.open(png_path).convert("RGBA")
    sizes = [16, 32, 48]

    dibs = [_make_dib(img.resize((s, s), Image.LANCZOS)) for s in sizes]

    k32 = ctypes.windll.kernel32
    _setup_api(k32)

    handle = k32.BeginUpdateResourceW(exe_path, False)
    if not handle:
        raise OSError(
            f"BeginUpdateResource failed (error {ctypes.GetLastError()}). "
            "Is the .exe read-only or already running?"
        )

    try:
        # Write RT_ICON entries (IDs 1..N)
        for idx, dib in enumerate(dibs, start=1):
            _update(k32, handle, RT_ICON, idx, dib)

        # Write RT_GROUP_ICON entry (ID=1 → Explorer application icon)
        grp = struct.pack("<HHH", 0, 1, len(dibs))
        for idx, (dib, size) in enumerate(zip(dibs, sizes), start=1):
            grp += struct.pack("<BBBBHHIH", size, size, 0, 0, 1, 32, len(dib), idx)
        _update(k32, handle, RT_GROUP_ICON, 1, grp)

    except Exception:
        k32.EndUpdateResourceW(handle, True)  # discard on error
        raise

    if not k32.EndUpdateResourceW(handle, False):
        raise OSError(
            f"EndUpdateResource failed (error {ctypes.GetLastError()})"
        )

    print(f"embed_icon: icon embedded successfully into {exe_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"Usage: {sys.argv[0]} <exe_path> <png_path>")
    embed_icon(sys.argv[1], sys.argv[2])

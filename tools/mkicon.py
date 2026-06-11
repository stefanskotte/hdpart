#!/usr/bin/env python3
"""Generate HDPart.info — a classic (old-style) AmigaOS Workbench icon.

Emits a WBTOOL DiskObject with a single 4-colour planar image, compatible with
icon.library on Kickstart 2.04 and 3.x. The 4 colours are the standard OS2.0
Workbench palette: 0=grey (transparent backdrop), 1=black, 2=white, 3=blue.

Motif: a 3.5" hard-drive chassis with a GParted-style partition bar across it,
and the name "HDPART" underneath.

Run:  python3 tools/mkicon.py [output.info]   (default: tools/HDPart.info)
"""
import struct, sys

# ---- palette indices ------------------------------------------------------
GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3

W, H = 56, 28
px = [[GREY] * W for _ in range(H)]


def putp(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[y][x] = c


def hline(x0, x1, y, c):
    for x in range(x0, x1 + 1):
        putp(x, y, c)


def vline(x, y0, y1, c):
    for y in range(y0, y1 + 1):
        putp(x, y, c)


def fillrect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            putp(x, y, c)


def rect(x0, y0, x1, y1, c):
    hline(x0, x1, y0, c)
    hline(x0, x1, y1, c)
    vline(x0, y0, y1, c)
    vline(x1, y0, y1, c)


# ---- artwork --------------------------------------------------------------
# Drive chassis: black outline, white face.
DX0, DY0, DX1, DY1 = 4, 3, 51, 25
fillrect(DX0, DY0, DX1, DY1, WHITE)
rect(DX0, DY0, DX1, DY1, BLACK)
# subtle top header line (drive lid seam)
hline(DX0 + 2, DX1 - 2, DY0 + 3, BLACK)
# activity LED (blue) top-right
fillrect(DX1 - 5, DY0 + 1, DX1 - 4, DY0 + 2, BLUE)

# Partition bar across the drive face: black-outlined box split into segments.
BX0, BY0, BX1, BY1 = 8, 12, 47, 21
fillrect(BX0, BY0, BX1, BY1, WHITE)
rect(BX0, BY0, BX1, BY1, BLACK)
# four partitions of differing fill, separated by black dividers
seg_fill = [BLUE, WHITE, BLUE, GREY]  # used / used / used / free
inner0, inner1 = BX0 + 1, BX1 - 1
span = inner1 - inner0 + 1
n = len(seg_fill)
edges = [inner0 + (span * i) // n for i in range(n)] + [inner1 + 1]
for i in range(n):
    sx0, sx1 = edges[i], edges[i + 1] - 1
    fillrect(sx0, BY0 + 1, sx1, BY1 - 1, seg_fill[i])
    if i > 0:
        vline(sx0 - 1, BY0 + 1, BY1 - 1, BLACK)  # divider


# ---- pack to planar bitplanes & write the .info ---------------------------
def planar(depth):
    bpr = ((W + 15) // 16) * 2  # bytes per row, word-aligned
    planes = bytearray()
    for plane in range(depth):
        bit = 1 << plane
        for y in range(H):
            row = bytearray(bpr)
            for x in range(W):
                if px[y][x] & bit:
                    row[x >> 3] |= 0x80 >> (x & 7)
            planes += row
    return bytes(planes), bpr


DEPTH = 2
imgdata, _ = planar(DEPTH)

out = bytearray()
# DiskObject header + embedded Gadget (old-style, big-endian)
out += struct.pack(">HH", 0xE310, 1)                 # do_Magic, do_Version
out += struct.pack(">I", 0)                           # gg_NextGadget
out += struct.pack(">hhhh", 0, 0, W, H)               # Left, Top, Width, Height
out += struct.pack(">HHH", 0x0004, 0x0003, 0x0001)    # Flags(GADGIMAGE), Activation, Type(BOOL)
out += struct.pack(">I", 1)                           # gg_GadgetRender (nonzero => image present)
out += struct.pack(">I", 0)                           # gg_SelectRender
out += struct.pack(">I", 0)                           # gg_GadgetText
out += struct.pack(">i", 0)                           # gg_MutualExclude
out += struct.pack(">I", 0)                           # gg_SpecialInfo
out += struct.pack(">H", 0)                           # gg_GadgetID
out += struct.pack(">I", 0)                           # gg_UserData
out += struct.pack(">BB", 3, 0)                       # do_Type=WBTOOL, pad
out += struct.pack(">I", 0)                           # do_DefaultTool
out += struct.pack(">I", 0)                           # do_ToolTypes
out += struct.pack(">II", 0x80000000, 0x80000000)    # do_CurrentX/Y = NO_ICON_POSITION
out += struct.pack(">I", 0)                           # do_DrawerData
out += struct.pack(">I", 0)                           # do_ToolWindow
out += struct.pack(">i", 4096)                        # do_StackSize

# Image structure
out += struct.pack(">hhHHH", 0, 0, W, H, DEPTH)       # Left, Top, Width, Height, Depth
out += struct.pack(">I", 1)                           # ImageData (nonzero)
out += struct.pack(">BB", (1 << DEPTH) - 1, 0)        # PlanePick, PlaneOnOff
out += struct.pack(">I", 0)                           # NextImage
out += imgdata

path = sys.argv[1] if len(sys.argv) > 1 else "tools/HDPart.info"
with open(path, "wb") as f:
    f.write(out)
print(f"wrote {path}  ({len(out)} bytes, {W}x{H}x{DEPTH})")

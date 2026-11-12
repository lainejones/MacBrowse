#!/usr/bin/env python3
"""
Generate the MacBrowse.info tool icon (adapted from C:/projects/tools/makeicon.py).

No external tools required. Produces a WBTOOL DiskObject with a single
2-bitplane Image, drawn procedurally (a compact Mac + arrow) in the
standard 4-colour Workbench palette:
    0 = grey (background)   1 = black   2 = white   3 = blue
The icon highlights by complement (GADGHCOMP) so no second image is needed.
"""
import struct, sys

W, H = 48, 40                       # 48 is a multiple of 16 -> clean rows
ROWWORDS = (W + 15) // 16           # = 3
ROWBYTES = ROWWORDS * 2             # = 6
PLANESIZE = ROWBYTES * H
DEPTH = 2

# ---- pixel canvas -------------------------------------------------------
px = [[0] * W for _ in range(H)]

def setp(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[y][x] = c

def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            setp(x, y, c)

def frame(x0, y0, x1, y1, c):
    for x in range(x0, x1 + 1):
        setp(x, y0, c); setp(x, y1, c)
    for y in range(y0, y1 + 1):
        setp(x0, y, c); setp(x1, y, c)

def disc(cx, cy, r, c):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                setp(x, y, c)

def ring(cx, cy, r, w, c):
    inner = r - w
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if inner * inner <= d <= r * r:
                setp(x, y, c)

# compact Macintosh --------------------------------------------------------
rect(6, 2, 29, 31, 2)               # white case
frame(6, 2, 29, 31, 1)
rect(9, 5, 26, 18, 3)               # blue screen
frame(9, 5, 26, 18, 1)
# little smiling face on the screen
for x, y in ((14, 9), (14, 10), (21, 9), (21, 10)): setp(x, y, 2)
for x in range(14, 22): setp(x, 14, 2)
setp(13, 13, 2); setp(22, 13, 2)
rect(18, 22, 26, 23, 1)             # floppy slot
rect(8, 32, 27, 35, 1)              # foot
rect(9, 32, 26, 34, 2)
# arrow (copy across) ------------------------------------------------------
for y in range(22, 27):
    for x in range(31, 40): setp(x, y, 3)
for i in range(7):
    for y in range(24 - i, 25 + i):
        setp(46 - i, y, 3)
for i in range(7):
    setp(46 - i, 24 - i, 1); setp(46 - i, 24 + i, 1)
frame(31, 21, 39, 27, 1)
for y in range(22, 27): setp(39, y, 3)

# ---- pack into 2 bitplanes ---------------------------------------------
def planes():
    data = bytearray()
    for p in range(DEPTH):
        for y in range(H):
            for wd in range(ROWWORDS):
                word = 0
                for bit in range(16):
                    x = wd * 16 + bit
                    val = px[y][x] if x < W else 0
                    if (val >> p) & 1:
                        word |= 1 << (15 - bit)
                data += struct.pack(">H", word)
    return bytes(data)

# ---- structures ---------------------------------------------------------
WB_DISKMAGIC, WB_DISKVERSION = 0xE310, 1
WBTOOL = 3
GFLG_GADGIMAGE = 0x0004            # render is an Image; GADGHCOMP highlight = 0
GACT = 0x0003                      # RELVERIFY | GADGIMMEDIATE
GTYP_BOOL = 0x0001
NO_ICON_POSITION = 0x80000000
PLACE = 0x00000064                 # non-zero "pointer present" placeholder

def gadget():
    return struct.pack(">IhhhhHHHIIIiIHI",
        0,            # NextGadget
        0, 0,         # LeftEdge, TopEdge
        W, H,         # Width, Height
        GFLG_GADGIMAGE,
        GACT,
        GTYP_BOOL,
        PLACE,        # GadgetRender (Image present)
        0,            # SelectRender
        0,            # GadgetText
        0,            # MutualExclude
        0,            # SpecialInfo
        0,            # GadgetID
        0)            # UserData

def disk_object(stack):
    return (struct.pack(">HH", WB_DISKMAGIC, WB_DISKVERSION)
        + gadget()
        + struct.pack(">BB", WBTOOL, 0)        # do_Type + pad
        + struct.pack(">I", 0)                 # do_DefaultTool (none)
        + struct.pack(">I", 0)                 # do_ToolTypes (none)
        + struct.pack(">II", NO_ICON_POSITION, NO_ICON_POSITION)
        + struct.pack(">I", 0)                 # do_DrawerData
        + struct.pack(">I", 0)                 # do_ToolWindow
        + struct.pack(">i", stack))            # do_StackSize

def image():
    return struct.pack(">hhhhhIBBI",
        0, 0,          # LeftEdge, TopEdge
        W, H, DEPTH,
        PLACE,         # ImageData (present)
        0x03,          # PlanePick (both planes from data)
        0x00,          # PlaneOnOff
        0)             # NextImage

def build(stack):
    return disk_object(stack) + image() + planes()

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "MacBrowse.info"
    with open(path, "wb") as f:
        f.write(build(16384))
    print("wrote", path)

if __name__ == "__main__":
    main()

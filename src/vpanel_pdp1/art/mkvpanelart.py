#!/usr/bin/env python3
"""
Generates the art for vpanel_pdp1, all at twice vpanel_pdp1's logical size so it stays sharp when
the window is scaled up, vpanel_pdp1 draws each piece into a rectangle of half its pixel size:

  lamp_on.png, lamp_off.png
      The lamps, lit like a glowing bulb.

  switch_u.png, switch_d.png, switch_l.png, switch_r.png
      Toggle switches, with a dark grey lever so the switch position is easy to see.

  key_n.png, key_d.png, key_u.png
      Momentary keys, neutral, pressed down and lifted up, the pressed ones with a wide dark grey lever.

  pdp1_panel_2x.png
      The console background.
      Made from pdp1_panel.png, the original 800x448 art: the lines are doubled pixel for pixel,
      they are all horizontal and vertical so they stay sharp,
      and the labels are erased and drawn again at the higher resolution, see CONSOLE_LABELS.

  io_panel.png
      The rack top (I/O) indicator panel background, labels and bracket bars only, the lamps
      are drawn by vpanel_pdp1. It is laid out after the PiDP-1's top panel,
      tape reader buffer, reader status, memory, typewriter and punch, drawn in the style of
      the console art.

and then remakes panelart.inc, pdp1art.inc and ioart.inc from the pngs with xxd -i.

It also writes the web server's front panel, web_pdp1/srv/panel/: a copy of the pngs and layout.json,
every lamp, switch and key's place and bit, worked out from ../elements.inc and the grid in ../main.c's
init() exactly as vpanel_pdp1 places them, so the two front ends always agree.

This only needs to be run if the art changes, the generated pngs and .inc files are committed.
It needs python3 with PIL, xxd, and the Nimbus Sans Narrow Bold font.

The I/O panel geometry below MUST match IO_* in ../main.c and the
iolamps[] table in ../elements.inc.
ART_SCALE MUST match ART_SCALE in ../main.c.

6-Nov-2026 Claude: initial version
6-Nov-2026 Claude: everything at 2x, redraw the lamps and keys, redraw the console labels
6-Nov-2026 Claude: lit lamps look like glowing bulbs
13-Nov-2026 Claude: also write the web server's front panel art and layout.json
"""
import json
import os
import re
import shutil
import subprocess
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = "/usr/share/fonts/truetype/NimbusSansNarrow-Bold.otf"     # closest to the original lettering

ART_SCALE = 2           # the art is made at this multiple of vpanel_pdp1's logical pixels
SS = 8                  # sprites are drawn at this multiple of ART_SCALE, then reduced, for antialiasing

# Colors, from the original art.
BG = (230, 230, 230, 255)
INK = (0, 0, 0, 255)
EDGE = (174, 174, 174, 255)
BEZEL = (208, 208, 208, 255)
LAMP_ON = (255, 191, 0, 255)       # flat lit color, the edge of the glow
LAMP_HOT = (255, 250, 210, 255)    # the glow's center
LAMP_MID = (255, 218, 70, 255)     # the glow halfway out
LAMP_RIM = (196, 132, 0, 255)      # a lit lamp's rim
RIM = (0, 0, 0, 150)
RING = (93, 93, 93, 255)
LEVER = (0x40, 0x40, 0x40, 255)
OUTLINE = (0, 0, 0, 255)

# Console label text, cap height in logical pixels.
CAP_HEIGHT = 5.6

# I/O panel geometry, in vpanel_pdp1 logical pixels, the panel is 800 wide like the console art.
IO_W = 800
IO_H = 220
IO_XSCL = 12.95 * 800.0 / 500.0         # lamp pitch, same as the console's grid1
IO_X0 = IO_W / 2.0 - 8.5 * IO_XSCL      # column 0 lamp center, centers the 18 columns
IO_Y0 = 33.0                            # row 0 lamp center
IO_YSCL = 52.0                          # row pitch

# Offsets from an I/O panel lamp row's center, logical pixels.
GROUP_BASELINE = -17        # group label text baseline
BAR_Y = -13                 # bracket bar
TICK_LEN = 6                # bracket end ticks, downward from the bar
NAME_BASELINE = 17          # lamp name text baseline
IO_LETTER_SPACING = 0.5     # about the console labels' spacing, the script prints it

# The console labels, the ink bounding box of each in pdp1_panel.png, x0, y0, x1, y1 inclusive, and its text.
# Each is drawn again with its letter spacing fitted so it covers the same width, its baseline on the box's bottom.
CONSOLE_LABELS = (
    (133, 42, 175, 47, "EXTENSION"),
    (282, 42, 356, 47, "PROGRAM  COUNTER"),
    (507, 63, 522, 68, "RUN"),
    (665, 63, 690, 68, "POWER"),
    (133, 83, 175, 88, "EXTENSION"),
    (284, 83, 354, 88, "MEMORY  ADDRESS"),
    (507, 84, 529, 88, "CYCLE"),
    (665, 84, 716, 88, "SINGLE  STEP"),
    (507, 104, 530, 109, "DEFER"),
    (665, 104, 714, 109, "SINGLE  INST"),
    (225, 124, 289, 129, "MEMORY  BUFFER"),
    (508, 125, 557, 130, "H. S.  CYCLE"),
    (631, 141, 700, 146, "SENSE  SWITCHES"),
    (507, 146, 550, 150, "BRK. CTR.  1"),
    (230, 165, 285, 170, "ACCUMULATOR"),
    (507, 166, 551, 171, "BRK. CTR.  2"),
    (507, 187, 551, 192, "OVER  FLOW"),
    (612, 202, 613, 206, "1"),
    (632, 202, 635, 206, "2"),
    (653, 202, 656, 206, "3"),
    (673, 202, 676, 206, "4"),
    (694, 202, 697, 206, "5"),
    (715, 202, 718, 206, "6"),
    (243, 207, 273, 211, "IN - OUT"),
    (507, 208, 537, 212, "READ-IN"),
    (507, 228, 554, 233, "SEQ.  BREAK"),
    (634, 228, 698, 233, "PROGRAM  FLAGS"),
    (66, 248, 94, 253, "EXTEND"),
    (133, 248, 175, 253, "EXTENSION"),
    (300, 248, 335, 253, "ADDRESS"),
    (507, 249, 536, 253, "EXTEND"),
    (612, 258, 613, 262, "1"),
    (632, 258, 635, 262, "2"),
    (653, 258, 656, 262, "3"),
    (673, 258, 676, 262, "4"),
    (694, 258, 697, 262, "5"),
    (715, 258, 718, 262, "6"),
    (507, 269, 544, 274, "I - O  HALT"),
    (631, 278, 681, 283, "INSTRUCTION"),
    (236, 289, 278, 294, "TEST  WORD"),
    (507, 290, 556, 295, "I - O  COM'DS"),
    (507, 311, 546, 315, "I - O  SYNC"),
    (89, 365, 111, 370, "START"),
    (153, 365, 171, 370, "STOP"),
    (205, 365, 243, 370, "CONTINUE"),
    (270, 365, 303, 370, "EXAMINE"),
    (333, 365, 364, 370, "DEPOSIT"),
    (395, 365, 426, 370, "READ  IN"),
    (583, 365, 612, 370, "READER"),
    (638, 365, 680, 370, "TAPE  FEED"),
)


def font():
    # Size for CAP_HEIGHT, at the art scale.
    f = ImageFont.truetype(FONT, 100)
    capH = -f.getbbox("H", anchor="ls")[1]
    return ImageFont.truetype(FONT, round(100 * CAP_HEIGHT * ART_SCALE / capH))


# ---- sprites

class Sprite:
    """A sprite drawn in logical pixel coordinates on a supersampled canvas."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.k = ART_SCALE * SS
        self.im = Image.new("RGBA", (w * self.k, h * self.k), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    def ellipse(self, cx, cy, rx, ry, fill):
        k = self.k
        self.d.ellipse([(cx - rx) * k, (cy - ry) * k, (cx + rx) * k - 1, (cy + ry) * k - 1], fill=fill)

    def rect(self, x0, y0, x1, y1, fill):
        k = self.k
        self.d.rectangle([x0 * k, y0 * k, x1 * k - 1, y1 * k - 1], fill=fill)

    def ring(self, cx, cy, rx, ry, width, color, inside):
        self.ellipse(cx, cy, rx, ry, color)
        self.ellipse(cx, cy, rx - width, ry - width, inside)

    def image(self):
        return self.im.reduce(SS)


# The lamp and toggle switch disk, the original 16x18 sprites' geometry.
def disk(s, fill):
    s.ring(8.0, 9.0, 7.0, 6.6, 1.0, RIM, fill)


def lamp(fill):
    s = Sprite(16, 18)
    disk(s, fill)
    return s.image()


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(4))


def lit_lamp():
    """A lit lamp, a glowing bulb, pale yellow in the center fading to amber, with an amber rim."""
    s = Sprite(16, 18)
    rx, ry = 7.0, 6.6
    s.ellipse(8.0, 9.0, rx, ry, LAMP_RIM)
    rx, ry = rx - 1.0, ry - 1.0
    # Concentric ellipses from the edge in, edge to halfway amber to yellow, then to the hot center.
    steps = 40
    for i in range(steps):
        t = i / (steps - 1)
        c = mix(LAMP_ON, LAMP_MID, t / 0.5) if t < 0.5 else mix(LAMP_MID, LAMP_HOT, (t - 0.5) / 0.5)
        k = 1.0 - t * 0.85
        s.ellipse(8.0, 9.0, rx * k, ry * k, c)
    return s.image()


def toggle(direction):
    """A toggle switch with the lever pointing up, down, left or right."""
    s = Sprite(16, 18)
    disk(s, BEZEL)
    # Lever, 4 pixels of dark grey with a black outline, from the top edge to the pivot.
    s.rect(5, 0, 11, 11, OUTLINE)
    s.rect(6, 1, 10, 9, LEVER)
    im = s.image()

    if direction == "u":
        return im
    if direction == "d":
        # The disk is centered on the sprite, it stays in place.
        return im.transpose(Image.FLIP_TOP_BOTTOM)

    # The horizontal switches are 18x16, the vertical ones turned.
    # Pad to 18x18 first so the disk stays centered, then trim to 18x16.
    a = ART_SCALE
    sq = Image.new("RGBA", (18 * a, 18 * a), (0, 0, 0, 0))
    sq.paste(im, (1 * a, 0))
    sq = sq.transpose(Image.ROTATE_90 if direction == "l" else Image.ROTATE_270)
    return sq.crop((0, 1 * a, 18 * a, 17 * a))


def key(state):
    """A momentary key, n neutral, d pressed down, u lifted up."""
    s = Sprite(27, 27)
    s.ring(13.5, 13.5, 8.5, 8.5, 1.0, RIM, BEZEL)
    if state == "n":
        # The lever seen end on.
        s.ring(13.5, 13.5, 2.6, 2.6, 1.0, RING, BEZEL)
        return s.image()

    # Lever, 6 pixels of dark grey with a black outline, from the pivot to the bottom edge.
    s.rect(9.5, 11, 17.5, 27, OUTLINE)
    s.rect(10.5, 12, 16.5, 26, LEVER)
    im = s.image()
    if state == "u":
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    return im


# ---- text

def text_line(d, x, baseline, text, f, spacing):
    """Draw text with extra letter spacing, art pixel coordinates, x is the left of the first glyph's advance."""
    for ch in text:
        d.text((x, baseline), ch, font=f, fill=INK, anchor="ls")
        x += d.textlength(ch, font=f) + spacing


def ink_extent(text, f, spacing):
    """The ink left and right, from the advance start, of text drawn with text_line()."""
    scratch = Image.new("L", (4000, 200), 0)
    d = ImageDraw.Draw(scratch)
    x = 100.0
    for ch in text:
        d.text((x, 150), ch, font=f, fill=255, anchor="ls")
        x += d.textlength(ch, font=f) + spacing
    box = scratch.getbbox()
    return box[0] - 100.0, box[2] - 100.0


def fitted_text(d, x0, x1, baseline, text, f):
    """Draw text so its ink spans x0 to x1, art pixels, by fitting the letter spacing."""
    if len(text) == 1:
        l, r = ink_extent(text, f, 0)
        text_line(d, (x0 + x1) / 2.0 - (l + r) / 2.0, baseline, text, f, 0)
        return 0.0
    l, r = ink_extent(text, f, 0)
    spacing = ((x1 - x0) - (r - l)) / (len(text) - 1)
    l, r = ink_extent(text, f, spacing)
    text_line(d, x0 - l, baseline, text, f, spacing)
    return spacing


def centered_text(d, cx, baseline, text, f, spacing):
    """Draw text with the given letter spacing, its ink centered on cx, art pixels."""
    l, r = ink_extent(text, f, spacing)
    text_line(d, cx - (l + r) / 2.0, baseline, text, f, spacing)


# ---- console

def console_panel():
    a = ART_SCALE
    src = Image.open(os.path.join(HERE, "pdp1_panel.png")).convert("RGBA")
    im = src.resize((src.width * a, src.height * a), Image.NEAREST)
    d = ImageDraw.Draw(im)
    f = font()

    spacings = []
    for x0, y0, x1, y1, text in CONSOLE_LABELS:
        # Erase the old label, a pixel around its box for its antialiasing.
        d.rectangle([(x0 - 1) * a, (y0 - 1) * a, (x1 + 2) * a - 1, (y1 + 2) * a - 1], fill=BG)
        sp = fitted_text(d, x0 * a, (x1 + 1) * a, (y1 + 1) * a, text, f)
        if len(text) > 1:
            spacings.append(sp / a)

    spacings.sort()
    print("console label letter spacing, logical pixels: median %.2f, %.2f to %.2f"
          % (spacings[len(spacings) // 2], spacings[0], spacings[-1]))
    return im


# ---- I/O panel

# The I/O panel's layout, (row, column, label) for each lamp, and the bracketed groups.
# Row and column are lamp grid positions, column 0-17.
def io_lamps():
    lamps = []
    for c in range(18):
        lamps.append((0, c, str(c)))
    for c, name in ((0, "BINARY"), (2, "NAC"), (4, "RC 1"), (6, "RC 2"), (8, "CLUTCH"),
                    (11, "READ"), (13, "STROBE"), (15, "INHIBIT"), (17, "WRITE")):
        lamps.append((1, c, name))
    for c, name in ((2, "STATUS"), (4, "BLACK"), (6, "TYO"), (8, "NAC")):
        lamps.append((2, c, name))
    for c in range(12, 18):
        lamps.append((2, c, str(c)))
    for c in (0, 1, 2, 3, 4, 6, 7, 8):
        lamps.append((3, c, ""))
    for c, name in ((10, "ON"), (12, "NAC"), (15, "DEFER 2"), (17, "OV 2")):
        lamps.append((3, c, name))
    return lamps


IO_GROUPS = (
    # row, first column, last column, label
    (0, 0, 17, "TAPE  READER"),
    (1, 0, 8, "TAPE  READER"),
    (1, 11, 17, "MEMORY"),
    (2, 2, 17, "TYPEWRITER"),
    (3, 0, 12, "TAPE  PUNCH"),
)


def io_panel():
    a = ART_SCALE
    im = Image.new("RGBA", (IO_W * a, IO_H * a), BG)
    d = ImageDraw.Draw(im)
    f = font()
    sp = IO_LETTER_SPACING * a

    def cx(c):
        return (IO_X0 + c * IO_XSCL) * a

    def cy(r):
        return (IO_Y0 + r * IO_YSCL) * a

    for r, c0, c1, label in IO_GROUPS:
        y = round(cy(r) + BAR_Y * a)
        x0 = round(cx(c0) - IO_XSCL / 2 * a)
        x1 = round(cx(c1) + IO_XSCL / 2 * a)
        d.rectangle([x0, y, x1 - 1, y + 2 * a - 1], fill=INK)
        d.rectangle([x0, y, x0 + 2 * a - 1, y + TICK_LEN * a - 1], fill=INK)
        d.rectangle([x1 - 2 * a, y, x1 - 1, y + TICK_LEN * a - 1], fill=INK)
        centered_text(d, (x0 + x1) / 2.0, cy(r) + GROUP_BASELINE * a, label, f, sp)

    for r, c, name in io_lamps():
        if name:
            centered_text(d, cx(c), cy(r) + NAME_BASELINE * a, name, f, sp)

    # The edge between this panel and the console below.
    d.rectangle([0, IO_H * a - 2 * a, IO_W * a, IO_H * a], fill=EDGE)

    return im


# ---- web front panel

WEB_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "..", "web_pdp1", "srv", "panel"))
ELEMENTS = os.path.join(HERE, "..", "elements.inc")
WEB_ART = ["pdp1_panel_2x", "io_panel", "lamp_on", "lamp_off", "switch_u", "switch_d", "switch_l", "switch_r",
           "key_n", "key_d", "key_u"]

# The console grids, as ../main.c's init() sets them, logical pixels, the console's own top at 0.
SCL = 800.0 / 500.0
GRID1 = dict(xscl=12.95 * SCL, yscl=12.9 * SCL, xoff=4.5 * SCL + 12.95 * SCL / 2, yoff=2.56 * SCL)
GRID2 = dict(GRID1, xoff=1.05 * SCL + 12.95 * SCL / 2)
GRID3 = dict(xscl=IO_XSCL, yscl=IO_YSCL, xoff=IO_X0, yoff=IO_Y0)

# Logical sprite sizes, the art's size over ART_SCALE.
SIZES = {"lamptex": (16, 18), "switchtex": (16, 18), "hswitchtex": (18, 16), "keytex": (27, 27)}

# ../main.c's init() carves lights[] and switches[] into groups by count, and updatepanel() gives each group
# a Panel word and the bit of its first element, each next element one bit lower.
# These MUST match main.c; the counts are checked against elements.inc below.
LAMP_GROUPS = [  # count, lights row, first bit
    (16, 0, 0o100000), (16, 1, 0o100000), (18, 2, 0o400000), (18, 3, 0o400000), (18, 4, 0o400000),
    (13, 5, 0o400000), (3, 5, 0o000004), (6, 6, 0o004000), (6, 6, 0o000040), (5, 6, 0o400000)]
SWITCH_GROUPS = [  # count, sw word, first bit, or a list of (word, bit) one per element
    (1, [(0, 0o400000)]),                                   # EXTEND
    (16, 0, 0o100000),                                      # TA
    (18, 1, 0o400000),                                      # TW
    (3, [(0, 0o200000), (2, 0o400000), (2, 0o200000)]),     # POWER, SINGLE STEP, SINGLE INST
    (6, 2, 0o100000)]                                       # sense switches
# Keys, in elements.inc order: the sw2 bit for pressed down and for lifted up, 0 for none.
KEY_BITS = [(0o001000, 0o000400), (0o000200, 0), (0o000100, 0), (0o000040, 0), (0, 0o000020),
            (0o000010, 0), (0o000004, 0o000002), (0o000001, 0)]


def parse_elements():
    """Read lights[], switches[], keys[] and iolamps[] from elements.inc."""
    arrays = {}
    current, grid = None, None
    for line in open(ELEMENTS):
        m = re.match(r"\s*(Element|IoLamp)\s+(\w+)\[\]", line)
        if m:
            current = m.group(2)
            arrays[current] = []
            continue
        m = re.match(r"#define G &(grid\d)", line)
        if m:
            grid = m.group(1)
            continue
        m = re.match(r"\s*\{\s*(\w+),\s*G,\s*([\d.]+),\s*([\d.]+),", line)
        if m and current:
            arrays[current].append((m.group(1), grid, float(m.group(2)), float(m.group(3))))
            continue
        m = re.match(r"\s*\{\s*([\d.]+),\s*([\d.]+),\s*(\d+),\s*(0[0-7]+)\s*\}", line)
        if m and current == "iolamps":
            arrays[current].append((float(m.group(1)), float(m.group(2)), int(m.group(3)), int(m.group(4), 8)))
    return arrays


def place(tex, grid, x, y):
    """The element's rectangle, as putongrid() in ../main.c makes it: C float to int truncation, int w/2."""
    g = {"grid1": GRID1, "grid2": GRID2, "grid3": GRID3}[grid]
    w, h = SIZES[tex]
    return [int(g["xoff"] - w // 2 + g["xscl"] * x), int(g["yoff"] - h // 2 + g["yscl"] * y), w, h]


def expand(groups, total):
    """One (word, bit) per element from a group table."""
    out = []
    for grp in groups:
        if isinstance(grp[1], list):
            out += grp[1]
        else:
            n, word, bit = grp
            out += [(word, bit >> i) for i in range(n)]
    assert len(out) == total, "group table has %d elements, elements.inc %d" % (len(out), total)
    return out


def web_layout():
    a = parse_elements()
    lamps = [dict(r=place(t, g, x, y), row=w, mask=b)
             for (t, g, x, y), (w, b) in zip(a["lights"], expand(LAMP_GROUPS, len(a["lights"])))]
    switches = [dict(r=place(t, g, x, y), word=w, mask=b, horiz=(t == "hswitchtex"))
                for (t, g, x, y), (w, b) in zip(a["switches"], expand(SWITCH_GROUPS, len(a["switches"])))]
    assert len(a["keys"]) == len(KEY_BITS)
    keys = [dict(r=place(t, g, x, y), down=d, up=u) for (t, g, x, y), (d, u) in zip(a["keys"], KEY_BITS)]
    iolamps = [dict(r=place("lamptex", "grid3", x, y), row=row, mask=mask) for x, y, row, mask in a["iolamps"]]
    return dict(consoleW=IO_W, consoleH=448, ioH=IO_H, artScale=ART_SCALE,
                lamps=lamps, switches=switches, keys=keys, iolamps=iolamps)


def web_panel():
    os.makedirs(WEB_DIR, exist_ok=True)
    for n in WEB_ART:
        shutil.copyfile(os.path.join(HERE, n + ".png"), os.path.join(WEB_DIR, n + ".png"))
    with open(os.path.join(WEB_DIR, "layout.json"), "w") as f:
        json.dump(web_layout(), f, indent=1)
        f.write("\n")


def xxd(names, out):
    with open(os.path.join(HERE, out), "w") as f:
        for n in names:
            f.write(subprocess.run(["xxd", "-i", n + ".png"], cwd=HERE, check=True,
                                   capture_output=True, text=True).stdout)


def save(im, name):
    im.save(os.path.join(HERE, name + ".png"), optimize=True)


def main():
    save(lit_lamp(), "lamp_on")
    save(lamp(BEZEL), "lamp_off")
    for direction in "udlr":
        save(toggle(direction), "switch_" + direction)
    for state in "ndu":
        save(key(state), "key_" + state)
    save(console_panel(), "pdp1_panel_2x")
    save(io_panel(), "io_panel")

    # Same order as the original panelart.inc.
    xxd(["key_n", "key_d", "key_u", "lamp_on", "lamp_off", "switch_d", "switch_u", "switch_r", "switch_l"],
        "panelart.inc")
    xxd(["pdp1_panel_2x"], "pdp1art.inc")
    xxd(["io_panel"], "ioart.inc")
    web_panel()
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""make_fixtures.py -- writes the F1 to F11 fixtures of the overlay suite, run by run_tests.sh.

Every F1 and F8 fixture is f1_valid.am1 with one change, and every F2 fixture
f2_valid.am1 with one, so that each fails, warns or passes for the one reason
its MANIFEST line names.  f1_valid and f2_valid are the controls: each must
assemble, with the words checked in its MANIFEST lines.  F3 is one program,
f3_ovcall.am1, whose words are worked out by hand in F3_REF below.  F4 is the
drum layout, worked out by hand in the comments; F5 the table, the headers
and the stamp, and F6 the .ovl, .osym and .sym files, both from one program
whose words are worked out by hand, the stamp by stamp_hash(), which follows
ovlcodegen.c's definition.  F7 is that program again with overlay.ah's defines
building the same words beside am1's, a control with defines swapped, and the
manager, overlay.ac, assembled in a program.  F9 is -S's figure per bank, the
highest word, in programs without overlays, each figure worked out by hand.
F10 is the optimizer on a program with overlays: the root alone is rewritten,
and every overlay word naming a root word that moves follows it, each word
worked out by hand above F10; each variant of f10_pins changes what the
overlay names, and f10_window and f10_windowhigh keep a window clear.
F11 is the optimizer with each overlay in a view of its own: f11_twins's two
overlays of one window each take their own rewrite, worked out by hand above
F11, and its variants open a region at an overlay's boundary; f11_pins keeps
another overlay's pin on a root word, and f11_nopin, without it, is the control.
f11_shrink deletes a word in an overlay, its files checked against
hand/f11_shrink.am1, the deletion made by hand; f11_full's unroll does not
fit its window, and f11_roomy's, with a window two words longer, does.

Run it from anywhere: it writes beside itself.  The fixtures are regenerated,
not edited: change the base or a variant here and run it again.

    python3 -I make_fixtures.py
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))

# The control.  Two windows in bank 2 and three overlays; an overlay's
# forward reference to a root global (helper) and a backward one (count); a
# name (wk) that two overlays each define for themselves; a var in the
# persistent overlay, which must not warn.
BASE = """\
f1_valid: two windows, three overlays, the control for every F1 and F8 variant
%%drumreserve 0-17, 20
bank 2
count,  0
%%window w1, 2000, 2777
%%window w2, 3000, 3077
%%overlay edit, w1
        export editRun
editRun, dap editX
        lac count
        add [1]
        dac count
editX,  jmp .
wk,     0
%%endoverlay
%%overlay print, w1, persistent
        export printAll
printAll, dap printX
        var hits
        idx hits
        jsp helper
printX, jmp .
wk,     0
%%endoverlay
%%overlay small, w2
        export smallRun
smallRun, dap smallX
smallX, jmp .
%%endoverlay
100/
helper, jmp .
bank 0
100/
go,     hlt
%%ovtable
        start go
"""

# name, list of (old, new) changes to BASE, MANIFEST lines (expect|flags|text)
VARIANTS = [
    # Overlay structure
    ("f1_nested", [("%%overlay edit, w1\n", "%%overlay edit, w1\n%%overlay inner, w2\n")],
        ["fail||overlays do not nest"]),
    ("f1_inlocal", [("%%overlay edit, w1\n", "        local tmp\n%%overlay edit, w1\n")],
        ["fail||inside a local scope; an overlay may not begin"]),
    ("f1_nowindow", [("%%overlay edit, w1\n", "%%overlay edit, w9\n")],
        ["fail||names window w9, which is not declared"]),
    ("f1_windowin", [("%%overlay edit, w1\n", "%%overlay edit, w1\n%%window w4, 3100, 3177\n")],
        ["fail||a window is declared in the root"]),
    ("f1_strayend", [("wk,     0\n%%endoverlay\n%%overlay print", "wk,     0\n%%endoverlay\n%%endoverlay\n%%overlay print")],
        ["fail||%%endoverlay with no overlay open"]),
    # The overlay runs to the end of the source: the root's origins after it
    # would be refused first, inside it.
    ("f1_unclosed", [("smallX, jmp .\n%%endoverlay\n100/\nhelper, jmp .\nbank 0\n100/\ngo,     hlt\n%%ovtable\n",
                      "smallX, jmp .\n")],
        ["fail||is never closed with %%endoverlay"]),
    ("f1_crossfile", [("wk,     0\n%%endoverlay\n%%overlay print", "wk,     0\n#include \"f1_endoverlay.ah\"\n%%overlay print")],
        ["fail||an overlay may not cross a file boundary"]),
    ("f1_bank", [("        lac count\n", "        lac count\nbank 3\n")],
        ["fail||bank cannot be used inside overlay edit"]),
    ("f1_origin", [("        lac count\n", "        lac count\n2100/\n")],
        ["fail||an origin cannot be used inside overlay edit"]),
    ("f1_import", [("        lac count\n", "        lac count\nimport \"nofile.sym\"\n")],
        ["fail||an import cannot be used inside overlay edit"]),
    ("f1_localopen", [("wk,     0\n%%endoverlay\n%%overlay print", "wk,     0\n        local tmp\n%%endoverlay\n%%overlay print")],
        ["fail||overlay edit ends inside a local scope"]),
    ("f1_toolong", [("%%window w2, 3000, 3077\n", "%%window w2, 3000, 3002\n")],
        ["fail||overlay small is 4 words, its 2 header words included, and window w2 holds 3"]),

    # Windows
    ("f1_firstlast", [("%%window w2, 3000, 3077\n", "%%window w2, 3077, 3000\n")],
        ["fail||window w2's first address 3077 is above its last, 3000"]),
    ("f1_tiny", [("%%window w2, 3000, 3077\n", "%%window w2, 3000, 3001\n")],
        ["fail||window w2 is 2 words; it must hold an overlay's 2 header words"]),
    ("f1_bank0high", [("%%ovtable\n", "%%window w5, 7700, 7760\n%%ovtable\n")],
        ["fail||window w5 ends at 7760 in bank 0, above 07750"]),
    ("f1_banktag", [("%%window w2, 3000, 3077\n", "%%window w2, 3000:3, 3077\n")],
        ["fail||window w2 is declared in bank 2 but given address 33000, in bank 3"]),
    ("f1_overlap", [("%%window w2, 3000, 3077\n", "%%window w2, 2700, 3077\n")],
        ["fail||window w2 overlaps window w1, 2000-2777 in bank 2"]),
    ("f1_unknown", [("%%window w2, 3000, 3077\n", "%%window w2, wstart, 3077\n")],
        ["fail||names wstart, which has no value yet"]),
    ("f1_dupwindow", [("%%window w2, 3000, 3077\n", "%%window w1, 3000, 3077\n")],
        ["fail||window w1 is already declared"]),

    # Overlay names and modes
    ("f1_dupoverlay", [("%%overlay small, w2\n", "%%overlay edit, w2\n")],
        ["fail||overlay edit is already declared"]),
    ("f1_badmode", [("%%overlay print, w1, persistent\n", "%%overlay print, w1, resident\n")],
        ["fail||overlay print: 'resident' is not a valid mode"]),

    # The table
    ("f1_ovtablein", [("        lac count\n", "        lac count\n%%ovtable\n")],
        ["fail||the table is in the root"]),
    ("f1_ovtabletwice", [("%%ovtable\n", "%%ovtable\n%%ovtable\n")],
        ["fail||%%ovtable is given twice"]),
    ("f1_ovtableearly", [("%%window w2, 3000, 3077\n", "%%window w2, 3000, 3077\n%%ovtable\n")],
        ["fail||%%ovtable comes before any overlay"]),
    ("f1_ovtableovl", [("%%overlay small, w2\n", "%%ovtable\n%%overlay small, w2\n")],
        ["fail||%%overlay small follows the %%ovtable"]),
    ("f1_ovtablewin", [("%%ovtable\n", "%%ovtable\n%%window w6, 3100, 3177\n")],
        ["fail||%%window w6 follows the %%ovtable"]),

    # Drum tracks
    ("f1_trackhigh", [("%%drumreserve 0-17, 20\n", "%%drumreserve 0-17, 40\n")],
        ["fail||%%drumreserve names track 40; the drum's tracks are 0-37"]),
    ("f1_trackback", [("%%drumreserve 0-17, 20\n", "%%drumreserve 17-0, 20\n")],
        ["fail||%%drumreserve range 17-0 runs backward"]),

    # Names shared with the root
    ("f1_labelroot", [("editX,  jmp .\n", "editX,  jmp .\ncount,  0\n")],
        ["fail||label count in overlay edit has the name of a global of bank 2's root"]),
    ("f1_labelrootref", [("count,  0\n", "count,  0\n        jmp later\n"),
                         ("editX,  jmp .\n", "editX,  jmp .\nlater,  0\n")],
        ["fail||label later in overlay edit has a name the root of bank 2 refers to"]),
    ("f1_varroot", [("        lac count\n", "        lac count\n        var count\n")],
        ["fail||var count in overlay edit has the name of a global of bank 2's root"]),
    ("f1_exportroot", [("        export editRun\n", "        export editRun, count\n")],
        ["fail||an overlay exports only its own symbols"]),
    ("f1_exportundef", [("        export editRun\n", "        export editRun, ghost\n")],
        ["fail||overlay edit exports ghost, which it never defines"]),
    ("f1_rootredef", [("helper, jmp .\n", "helper, jmp .\neditX,  0\n")],
        ["fail||editX is defined in the root of bank 2, and in overlay edit"]),
    # An overlay word is evaluated in every assembly, the tape's too.
    ("f1_undefined", [("        jsp helper\n", "        jsp nowhere\n")],
        ["fail||symbol nowhere has no defined value",
         "fail|bin|symbol nowhere has no defined value"]),

    # -O, -O1 and -O2, accepted since stage A of the optimizer task (F10 tests
    # what they do), and -d, accepted since am1 v3.7: its listing is made,
    # overlay headings and all
    ("f1_optimize", [], ["ok|-O|", "ok|-O1|", "ok|-O2|", "optgrep|-O|^This program has drum overlays"]),
    ("f1_debug", [], ["lstgrep|bin -d|^// Overlay print, number 2, in window w1, bank 2, 2000-2777, persistent$"]),

    # F8: the read-only var warning
    ("f8_varro", [("%%overlay print, w1, persistent\n", "%%overlay print, w1\n")],
        ["warn||a var in overlay print is read-only"]),
]

# The control's own lines: it assembles; the forward reference from an overlay
# to a root global defined after it (jsp helper, helper at 2:0100) and the
# backward one (lac count, count at 2:0000) carry the root's addresses; the
# persistent overlay's var does not warn.  An overlay's words are
# tagged with its name in the dump, and the pools %%endoverlay places are
# dumped, edit's literal 1 after its last word (wk, 2:2007) and print's var
# hits after its (wk, 2:2006); the listing gives an overlay's words their
# values, heads each overlay, lists its pools and its length, and lists the
# %%drumreserve's tracks as one set (0-17 and 20 octal are 0-20).
CONTROL = [
    "ok||",
    "okgrep||^022004 620100 print$",
    "okgrep||^022003 200000 edit$",
    "okgrep||^022010 000001 edit$",
    "okgrep||^022007 000000 print$",
    "nowarn||read-only",
    "lstgrep|bin -l|^ +10: 022003 200000 lac count$",
    "lstgrep|bin -l|^// Overlay print, number 2, in window w1, bank 2, 2000-2777, persistent$",
    "lstgrep|bin -l|^ +15: 022010 000001 $",
    "lstgrep|bin -l|^ +24: 022007 000000 hits, 0$",
    "lstgrep|bin -l|%%endoverlay +// overlay edit: 11 words of window w1's 1000",
    "lstgrep|bin -l|^ +2: 000004 +%%drumreserve 0-20$",
]

INCLUDE = "%%endoverlay\n"

# The F2 control.  ovEnter in bank 1 stands in for overlay.ac's entry.  The
# root's last word below window w1 (edge1, 2:1777) and its first above w2
# (helper, 2:3100) are next to the windows, not in them.  %%ovcall is made
# from bank 1, the manager's, and from bank 0.  smallPriv is defined by overlay
# small and not exported.
F2_BASE = """\
f2_valid: the control for every F2 variant: windows, overlays and %%ovcall
bank 1
100/
ovEnter, jmp .
        %%ovcall print, printAll
        hlt
bank 2
count,  0
1777/
edge1,  0
%%window w1, 2000, 2777
%%window w2, 3000, 3077
%%overlay edit, w1
        export editRun
editRun, dap editX
        lac count
        jsp helper
editX,  jmp .
%%endoverlay
%%overlay print, w1, persistent
        export printAll
printAll, dap printX
printX, jmp .
%%endoverlay
%%overlay small, w2
        export smallRun
smallRun, dap smallX
smallX, jmp .
smallPriv, 0
%%endoverlay
3100/
helper, jmp .
bank 0
100/
go,     cla
        %%ovcall edit, editRun
        %%ovcall small, smallRun
        hlt
%%ovtable
        start go
"""

F2_VARIANTS = [
    # A root word in a window, which the binary generator finds; fatal with
    # or without -M.  A table's words are checked the same way.
    ("f2_rootword", [("edge1,  0\n", "edge1,  0\nedge2,  0\n")],
        ["fail|bin|A root word at 02000 in bank 2 would be in window w1, 02000-02777"]),
    ("f2_rootwordm", [("edge1,  0\n", "edge1,  0\nedge2,  0\n")],
        ["fail|bin -M|A root word at 02000 in bank 2 would be in window w1, 02000-02777"]),
    ("f2_roottable", [("3100/\nhelper", "3070/\n        table 4\n3100/\nhelper")],
        ["fail|bin|A root word at 03070 in bank 2 would be in window w2, 03000-03077"]),

    # References across the overlay boundary
    ("f2_crossref", [("smallX, jmp .\n", "smallX, jmp printX\n")],
        ["fail||overlay small names printX, a symbol of overlay print"]),
    ("f2_rootref", [("helper, jmp .\n", "helper, jmp editX\n")],
        ["fail||the root names editX, a symbol of overlay edit"]),
    ("f2_rootrefbank", [("        hlt\n%%ovtable\n", "        jmp editX\n%%ovtable\n")],
        ["fail||the root names editX, a symbol of overlay edit"]),
    ("f2_rootbref", [("        hlt\n%%ovtable\n", "        lac editRun:2\n%%ovtable\n")],
        ["fail||the root names editRun, a symbol of overlay edit"]),

    # %%ovcall
    ("f2_ovcallmissing", [("        %%ovcall small, smallRun\n", "        %%ovcall tiny, smallRun\n")],
        ["fail||%%ovcall names overlay tiny, which is not declared"]),
    ("f2_ovcallprivate", [("        %%ovcall small, smallRun\n", "        %%ovcall small, smallPriv\n")],
        ["fail||%%ovcall names smallPriv, which overlay small does not export"]),
    ("f2_ovcallnoentry", [("        %%ovcall small, smallRun\n", "        %%ovcall small, nothing\n")],
        ["fail||%%ovcall names nothing, which overlay small does not export"]),
    ("f2_ovcallin", [("        lac count\n", "        lac count\n        %%ovcall small, smallRun\n")],
        ["fail||%%ovcall small, smallRun is inside overlay edit; only the root calls an overlay"]),
    ("f2_ovcallearly", [("ovEnter, jmp .\n", "        jmp .\n"),
                        ("        hlt\n%%ovtable\n", "        hlt\novEnter, jmp .\n%%ovtable\n")],
        ["fail||%%ovcall print, printAll comes before ovEnter, the overlay manager's entry, is defined"]),
    ("f2_noovtable", [("%%ovtable\n", "")],
        ["fail||this program has overlays and no %%ovtable"]),
]

# The F2 control's own lines: it assembles under -T and to a tape, the tape
# being where a root word in a window is looked for.  The listing
# gives each %%ovcall word a line with its value, the data words saying what
# they hold (edit's entry at 0:0115, as F3_REF works out for alpha's).
F2_CONTROL = [
    "ok||",
    "ok|bin|",
    "lstgrep|bin -l|^ +36: 000102 630131     jsp i \\[ovEnter:1\\]$",
    "lstgrep|bin -l|^ +36: 000103 000115     115      // overlay edit's entry in ovTable$",
    "lstgrep|bin -l|^ +36: 000104 022002     22002      // editRun$",
]

# F3: the words of %%ovcall, from the manager's bank and from another.  Two
# windows in bank 2 and three overlays, numbered 1 to 3 in source order; the
# words are worked out in F3_REF.
F3 = """\
f3_ovcall: the words of %%ovcall from the manager's bank and from another
bank 1
100/
ovEnter, jmp .
        %%ovcall beta, betaGo
        hlt
bank 2
%%window wa, 1000, 1777
%%window wb, 2000, 2777
%%overlay alpha, wa
        export alphaGo
        0
alphaGo, jmp .
%%endoverlay
%%overlay beta, wb
        export betaGo
betaGo, jmp .
%%endoverlay
%%overlay gamma, wa
        export gammaGo
        0
        0
gammaGo, jmp .
%%endoverlay
bank 0
100/
go,     cla
        %%ovcall alpha, alphaGo
        %%ovcall gamma, gammaGo
        hlt
%%ovtable
        start go
"""

# The root's words, by hand.  The header is two words, so an overlay's first
# word is its window's first address plus 2: alphaGo 2:1003, betaGo 2:2002,
# gammaGo 2:1004.  The table is at 0:0112, after hlt at 0:0111: one stamp
# word, one per window (two), then four per overlay, so alpha's entry is at
# 0112+3 = 0115, beta's at 0121, gamma's at 0125, and the table ends at 0130.
# Bank 0's literal pool follows it, at 0:0131, holding ovEnter's full
# address, 1:0100 = 010100; both calls from bank 0 share it.
#   jmp 600000, jsp 620000, i 010000, eem 724074, cla 760200, hlt 760400.
F3_REF = """\
010100 600100
010101 620100
010102 000121
010103 022002
010104 760400
000100 760200
000101 724074
000102 630131
000103 000115
000104 021003
000105 724074
000106 630131
000107 000125
000110 021004
000111 760400
000131 010100
000100 000100
"""

F3_LINES = [
    "refsub||",
]

# F4: the drum layout.  First fit in source order, skipping reserved tracks;
# overlays whose windows share drum addresses, in any bank, need different
# tracks, and windows with disjoint ranges share them.  Reserved: 0-20 and 22.
# a (w1) takes 21; b (w1) finds 21 taken by a and 22 reserved, so 23; c (w2,
# disjoint from w1) shares 21; d (w3, bank 3, w1's range) finds 21 and 23
# taken, 22 reserved, so 24.  Each overlay is its header and one word, 3 words.
F4 = """\
f4_layout: two windows at one range in two banks, a disjoint window, reserved tracks
%%drumreserve 0-17, 20
%%drumreserve 22
bank 2
%%window w1, 2000, 2777
%%window w2, 3000, 3077
%%overlay a, w1
aGo,    hlt
%%endoverlay
%%overlay b, w1
bGo,    hlt
%%endoverlay
%%overlay c, w2
cGo,    hlt
%%endoverlay
bank 3
%%window w3, 2000, 2777
%%overlay d, w3
dGo,    hlt
%%endoverlay
bank 0
100/
go,     hlt
%%ovtable
        start go
"""

F4_LINES = [
    "outgrep|-S|^overlay a, number 1: track 21, drum 2000-2777, window w1 in bank 2, 3 words, read-only$",
    "outgrep|-S|^overlay b, number 2: track 23, drum 2000-2777, window w1 in bank 2, 3 words, read-only$",
    "outgrep|-S|^overlay c, number 3: track 21, drum 3000-3077, window w2 in bank 2, 3 words, read-only$",
    "outgrep|-S|^overlay d, number 4: track 24, drum 2000-2777, window w3 in bank 3, 3 words, read-only$",
    "lstgrep|bin -l|^// overlay d, number 4: track 24, drum 2000-2777, window w3 in bank 3",
    # Banks 2 and 3 hold windows and no root word: "none", not -1 as an octal
    # 037777777777, which v3.5 printed; bank 0's line is the control.
    "outgrep|-S|^Bank 2, none$",
    "outgrep|-S|^Bank 3, none$",
    "outgrep|-S|^Bank 0, 00124 \\(84 decimal\\)$",
]


def many_overlays(name, count):
    """count overlays of one window and no reserved track: 32 fit, the 33rd does not."""
    text = "%s: %d overlays of one window, no track reserved\nbank 2\n%%%%window w1, 2000, 2777\n" % (name, count)
    for n in range(1, count + 1):
        text += "%%%%overlay o%d, w1\no%dGo,   hlt\n%%%%endoverlay\n" % (n, n)
    text += "bank 0\n100/\ngo,     hlt\n%%ovtable\n        start go\n"
    return text


F4_MANY = [
    ("f4_fit", 32, ["outgrep|-S|^overlay o32, number 32: track 37, drum 2000-2777"]),
    # -O too: the error comes after the optimizer's report is closed
    ("f4_full", 33, ["fail||overlay o33 has no drum track",
                     "fail|-O|overlay o33 has no drum track"]),
]

# F5 and F6: the table, the headers, the stamp and the files, worked out by hand
# from this program.  Each overlay's words past its header, by hand:
#   hlt 760400, cla 760200, lac 200000; a literal goes after the overlay's last
#   word, at %%endoverlay.
# one (window wa, bank 1, 0400-0437): oneGo hlt 760400 at 1:0402, 123 at 1:0403.
# two (wa, persistent): twoGo cla 760200 at 1:0402.
# three (wb, bank 3, 0400-0417): threeGo 7 at 3:0402, lac [55] at 3:0403 is
#   200404, the literal 55 at 3:0404.
# Lengths, the two header words included: 4, 3 and 5.
F5 = """\
f5_table: the table words, the headers and the stamp, worked out by hand
%%drumreserve 0, 2
bank 1
%%window wa, 400, 437
%%overlay one, wa
        export oneGo
oneGo,  hlt
        123
%%endoverlay
%%overlay two, wa, persistent
        export twoGo
twoGo,  cla
%%endoverlay
bank 3
%%window wb, 400, 417
%%overlay three, wb
        export threeGo
threeGo, 7
        lac [55]
%%endoverlay
bank 0
100/
go,     hlt
%%ovtable
        start go
"""

# Each overlay: name, number, window (bank, first, last), persistent, its words
# past the header.
F5_OVERLAYS = [
    ("one", 1, ("wa", 1, 0o400, 0o437), False, [0o760400, 0o123]),
    ("two", 2, ("wa", 1, 0o400, 0o437), True, [0o760200]),
    ("three", 3, ("wb", 3, 0o400, 0o417), False, [0o7, 0o200404, 0o55]),
]

# The tracks, by hand.  Reserved 0 and 2: one takes 1; two (wa) finds 1 taken
# and 2 reserved, so 3; three (wb, bank 3, overlapping wa's range) finds 1 and
# 3 taken, so 4.  f5_track reserves 0-2: 3, 4 and 5, every dia word changed
# and no overlay word.
F5_TRACKS = {"f5_table": [1, 3, 4], "f5_word": [1, 3, 4], "f5_track": [3, 4, 5]}

F5_VARIANTS = [
    # One overlay word changed: 123 becomes 124.
    ("f5_word", [("        123\n", "        124\n")]),
    # The tracks changed, no overlay word: a table word changes.
    ("f5_track", [("%%drumreserve 0, 2\n", "%%drumreserve 0-2\n")]),
]

# go is hlt at 0:0100, so the table is at 0:0101: the stamp, one word per window
# (wa, wb), then four per overlay; 1 + 2 + 12 = 15 words, to 0:0117.
F5_TABLE_ADDR = 0o101
F5_WINDOWS = ["wa", "wb"]


def stamp_hash(words):
    """The build stamp, as ovlcodegen.c defines it: (31h + w + 1) mod 2^18 over
    the words, and 0 made 1."""
    h = 0
    for w in words:
        h = (h * 31 + w + 1) & 0o777777
    return h if h else 1


def f5_model(overlays, tracks):
    """The table and each overlay's whole image, header included, and the stamp."""
    table = [0] + [0] * len(F5_WINDOWS)
    for (name, number, (wname, bank, first, last), persistent, words), track in zip(overlays, tracks):
        table.append(0o400000 | (track << 12) | first)                 # dia, read
        table.append((len(words) + 2) & 0o7777)                         # dwc, length
        table.append((bank << 12) | first)                              # dcl
        resident = F5_TABLE_ADDR + 1 + F5_WINDOWS.index(wname)          # the window's word, bank 0
        table.append(resident | (0o400000 if persistent else 0))
    words = []
    for ov in overlays:
        words += ov[4]
    stamp = stamp_hash(words + table[1:])
    table[0] = stamp
    images = [[stamp, ov[1]] + ov[4] for ov in overlays]
    return stamp, table, images


def f5_ref(overlays, tracks):
    """The dump lines of the table and of every overlay word, header included."""
    stamp, table, images = f5_model(overlays, tracks)
    lines = ["%06o %06o" % (F5_TABLE_ADDR + i, w) for i, w in enumerate(table)]
    for ov, image in zip(overlays, images):
        bank, first = ov[2][1], ov[2][2]
        for i, w in enumerate(image):
            lines.append("%02o%04o %06o %s" % (bank, first + i, w, ov[0]))
    return stamp, "\n".join(lines) + "\n"


def f6_ovl(source, overlays, tracks):
    """The .ovl file Docs/UsingOverlays.md describes, as ovlcodegen.c writes it."""
    stamp, table, images = f5_model(overlays, tracks)
    out = ["%%am1 overlay file%%", "V1", "%s stamp %06o table %06o" % (source, stamp, F5_TABLE_ADDR)]
    for ov, track, image in zip(overlays, tracks, images):
        wname, bank, first, last = ov[2]
        out.append("overlay %s window %s bank %o first %o last %o track %o length %o mode %s"
                   % (ov[0], wname, bank, first, last, track, len(image), "rw" if ov[3] else "ro"))
        for i in range(0, len(image), 8):
            out.append(" ".join("%06o" % w for w in image[i:i + 8]))
    return "\n".join(out) + "\n"


def line_of(text, start):
    """The source line, from 1, of the line that starts with start."""
    for n, line in enumerate(text.split("\n"), 1):
        if line.startswith(start):
            return n
    raise ValueError(start)


def f6_osym(text, source):
    """The .osym file: each overlay's own symbols, name order within it, with the
    overlay's name after the .sym line."""
    out = ["%%am1 overlay symtab file%%", "V1", source]
    for name, label, addr in [("one", "oneGo", 0o10402), ("two", "twoGo", 0o10402), ("three", "threeGo", 0o30402)]:
        out.append("%06o X %s %d %s" % (addr, label, line_of(text, label + ","), name))
    return "\n".join(out) + "\n"


def f6_sym(text, source):
    """The .sym file: the root's symbols only, go and ovTable, both in bank 0."""
    out = ["%%am1 symtab file%%", "V3", source,
           "%06o G go %d" % (0o100, line_of(text, "go,")),
           "%06o G ovTable %d" % (F5_TABLE_ADDR, line_of(text, "%%ovtable"))]
    return "\n".join(out) + "\n"


# F7: overlay.ah's defines agree with am1's table and headers.  The F5 program
# with overlay.ah included and, in bank 0, the table again at F7TBL and the three
# headers at F7HDR, each word placed by the defines' offsets and made from their
# fields and F5's layout by hand (tracks 1, 3, 4; windows wa and wb; the table
# at 0101).  The stamp is a number here, stamp_hash()'s: the root's words are
# not in it, so it is F5's.  The pairs checks hold am1's words against these;
# f7_swapped, the control, swaps two entry offsets and the two header offsets,
# and its pairs must disagree.
F7TBL = 0o200
F7HDR = 0o240


def f7_source(title, swaps):
    stamp, table, images = f5_model(F5_OVERLAYS, F5_TRACKS["f5_table"])
    body = F5.split("\n", 1)[1].replace("        start go\n", "")
    out = [title, "#include <DRUM/overlay.ah>"]
    for name, value in swaps:
        out += ["#undef " + name, "#define %s %s" % (name, value)]
    out.append(body.rstrip("\n"))
    out += ["#define F7TBL %04o" % F7TBL, "#define F7HDR %04o" % F7HDR,
            "F7TBL+OV_TBL_STAMP/", "        %06o" % stamp,
            "F7TBL+OV_TBL_WINDOWS/", "        0", "        0"]
    for k, ((name, number, (wname, bank, first, last), persistent, words), track) in \
            enumerate(zip(F5_OVERLAYS, F5_TRACKS["f5_table"])):
        at = "F7TBL+OV_TBL_WINDOWS+%d+(%d*OV_ENT_WORDS)+" % (len(F5_WINDOWS), k)
        out += [at + "OV_ENT_DIA/",
                "        OV_DIA_READ | (%o << OV_TRACK_SHIFT) | (%o & OV_DIA_ADDR)" % (track, first),
                at + "OV_ENT_DWC/",
                "        %o & OV_DWC_LENGTH" % (len(words) + 2),
                at + "OV_ENT_DCL/",
                "        (%o << OV_BANK_SHIFT) | (%o & OV_DCL_ADDR)" % (bank, first),
                at + "OV_ENT_RES/",
                "        ((ovTable + OV_TBL_WINDOWS + %d) & OV_RES_ADDR)%s"
                % (F5_WINDOWS.index(wname), " | OV_PERSIST" if persistent else "")]
    for k, ov in enumerate(F5_OVERLAYS):
        at = "F7HDR+(%d*OV_HDR_WORDS)+" % k
        out += [at + "OV_HDR_STAMP/", "        %06o" % stamp,
                at + "OV_HDR_NUMBER/", "        %o" % ov[1]]
    out.append("        start go")
    return "\n".join(out) + "\n"


# The pairs: am1's words, then the fixture's, then how many.
F7_PAIRS = ["%06o %06o %o" % (F5_TABLE_ADDR, F7TBL, 1 + len(F5_WINDOWS) + 4 * len(F5_OVERLAYS))]
for _k, _ov in enumerate(F5_OVERLAYS):
    F7_PAIRS.append("%02o%04o/%s %06o 2" % (_ov[2][1], _ov[2][2], _ov[0], F7HDR + 2 * _k))

F7_SWAPS = [("OV_ENT_DWC", "2"), ("OV_ENT_DCL", "1"), ("OV_HDR_STAMP", "1"), ("OV_HDR_NUMBER", "0")]

# The manager itself, overlay.ac, assembled where a program would have it: in
# bank 1, called from there and from bank 0, its table in bank 0.
F7_MANAGER = """\
f7_manager: overlay.ac in bank 1, called from bank 1 and bank 0
bank 1
100/
#include <DRUM/overlay.ac>
go1,
        %%ovcall beta, betaGo
        hlt
bank 2
%%window wa, 1000, 1777
%%window wb, 2000, 2777
%%overlay alpha, wa
        export alphaGo
alphaGo, dac alphaX
        eem
        jmp i alphaX
alphaX, 0
%%endoverlay
%%overlay beta, wb, persistent
        export betaGo
betaGo, dac betaX
        eem
        jmp i betaX
betaX,  0
%%endoverlay
bank 0
100/
go,     eem
        jsp i [ovInit:1]
        %%ovcall alpha, alphaGo
        hlt
%%ovtable
        start go
"""


# F9: -S's figure for a bank is its highest word, assembled or reserved by a
# table, not where its counter ended; space an origin skips does not count, the
# constants and variables placed at a bank's end do.  Each figure is worked out
# by hand in the comment beside it.  f9_ascending is f9_backward's words with
# the origins in ascending order, the control that must print the same line.
F9 = [
    # 01000-01002, then 0100-0101: the highest is 01002; v3.8 printed 00101.
    ("f9_backward", """\
f9_backward: code at 01000, then at 0100 below it
1000/
go,     cla
        cma
        jmp low
100/
low,    hlt
        jmp go
        start go
""", ["outgrep|-S|^Bank 0, 01002 \\(514 decimal\\)$"]),
    ("f9_ascending", """\
f9_ascending: f9_backward's words, the origins ascending
100/
low,    hlt
        jmp go
1000/
go,     cla
        cma
        jmp low
        start go
""", ["outgrep|-S|^Bank 0, 01002 \\(514 decimal\\)$"]),
    # A table of 010 at 02000-02007, reserved, then 0100: the highest is 02007.
    # The origin 03000 at the end holds nothing and does not count; v3.8
    # printed 02777.
    ("f9_table", """\
f9_table: a reserved table above the code, and an empty origin at the end
2000/
buf,
        table 10
100/
go,     lac buf
        hlt
3000/
        start go
""", ["outgrep|-S|^Bank 0, 02007 \\(1031 decimal\\)$"]),
    # The constant 01234 and the variable v1, placed by am1 after the last
    # run, which begins at 03000 and holds nothing else: 03000 and 03001.
    ("f9_auto", """\
f9_auto: constants and variables placed by am1 after an empty origin
100/
        var v1
go,     lac [1234]
        dac v1
        hlt
3000/
        start go
""", ["outgrep|-S|^Bank 0, 03001 \\(1537 decimal\\)$"]),
    # Every word in bank 1: bank 0 holds none; v3.8 printed 00003, the counter's
    # start, less one.
    ("f9_bank1", """\
f9_bank1: every word in bank 1
bank 1
100/
go,     hlt
        jmp go
        start go
""", ["outgrep|-S|^Bank 0, none$", "outgrep|-S|^Bank 1, 00101 \\(65 decimal\\)$"]),
]


# F10: the optimizer on a program with overlays.  Stage A rewrites the root
# alone; a root word an overlay names is pinned, an entry and a word whose
# address is used; an overlay word naming a root word that moves follows it.
# Assembled without -O, by hand:
#   0100 ovEnter jmp .  0101 go dzm rootFwd  0102 pinA cla  0103 lac rootVal
#   0104 sza  0105 jmp goS  0106 dzm rootVal  0107-0111 %%ovcall (jsp ovEnter,
#   ov1's table entry, ov1Go)  0112 jmp ., which a halt would not be, since
#   a continue runs on into helper  0113 helper dap helpX  0114 cla
#   0115 lac rootVal  0116 helpX jmp .  0117 rootVal  0120 rootFwd
#   0121-0126 ovTable (the stamp, window w's word, ov1's four).
#   ov1, window w 01000-01777: 01000-01001 the header, 01002 ov1Go dap ov1X,
#   01003 lac rootVal (a backward reference), 01004 lac rootFwd (a forward one,
#   which the overlay holds as an alias), 01005 lac [rootVal], 01006 jsp helper,
#   01007 ov1X jmp ., 01010 the literal, rootVal's address.  Length 9 (011).
# -O1 deletes three words, all in regions: the cla at 0102 (T6), the jmp at 0105
# (T2, the sza becoming sza i) and helper's cla at 0114 (T6).  Then rootVal is
# at 0114, rootFwd 0115, ovTable 0116 and helpX 0113, and ov1's words follow.
F10 = """\
f10_pins: the root alone is optimized, and the overlay's words follow it
bank 0
100/
ovEnter, jmp .
go,     dzm rootFwd
%%optimize
pinA,   cla
        lac rootVal
        sza
        jmp goS
        dzm rootVal
goS,
        %%ovcall ov1, ov1Go
        jmp .
%%endoptimize
helper, dap helpX
%%optimize
        cla
        lac rootVal
helpX,  jmp .
%%endoptimize
rootVal, 0
%%window w, 1000, 1777
%%overlay ov1, w
        export ov1Go
ov1Go,  dap ov1X
        lac rootVal
        lac rootFwd
        lac [rootVal]
        jsp helper
ov1X,   jmp .
%%endoverlay
rootFwd, 0
%%ovtable
        start go
"""


def f10_table(table_addr, image):
    """ovTable's words and the stamp, as f5_model() works them out: window w is
    the one window, ov1 on track 0, read-only."""
    table = [0, 0, 0o400000 | 0o1000, len(image) + 2, 0o1000, table_addr + 1]
    stamp = stamp_hash(image + table[1:])
    table[0] = stamp
    return stamp, table


# ov1's words past its header, as assembled and after -O1.
F10_IMAGE = [0o261007, 0o200117, 0o200120, 0o201010, 0o620113, 0o601007, 0o117]
F10_IMAGE_O1 = [0o261007, 0o200114, 0o200115, 0o201010, 0o620111, 0o601007, 0o114]


def f10_lines():
    """The control's lines: assembled as written, then with -O1."""
    stamp, table = f10_table(0o121, F10_IMAGE)
    stamp1, table1 = f10_table(0o116, F10_IMAGE_O1)
    assert stamp != stamp1
    return ["ok||", "ok|-O1|", "ok|bin -O1|",
            # Without -O: the words as written, and the stamp line.
            "okgrep||^000102 760200$",
            "okgrep||^001003 200117 ov1$",
            "okgrep||^000110 000123$",
            "okgrep||^000126 000122$",
            "ovlgrep|bin|stamp %06o table 000121$" % stamp,
            # -O1: the root's rewrites.
            "okgrep|-O1|^000101 340115$",
            "okgrep|-O1|^000102 200114$",
            "okgrep|-O1|^000103 650100$",
            "okgrep|-O1|^000112 200114$",
            # %%ovcall's data words: ov1's table entry, 0116 + 2, and ov1Go.
            "okgrep|-O1|^000106 000120$",
            "okgrep|-O1|^000107 001002$",
            # ov1's words naming moved root words: backward, forward, the
            # literal's word and the call; the stamp, which changed with them;
            # and the table's pointer to window w's word.
            "okgrep|-O1|^001003 200114 ov1$",
            "okgrep|-O1|^001004 200115 ov1$",
            "okgrep|-O1|^001010 000114 ov1$",
            "okgrep|-O1|^001006 620111 ov1$",
            "okgrep|-O1|^001000 %06o ov1$" % stamp1,
            "okgrep|-O1|^000123 000117$",
            "ovlgrep|bin -O1|stamp %06o table 000116$" % stamp1,
            "ovlgrep|bin -O1|^%s$" % " ".join("%06o" % w for w in [stamp1, 1] + F10_IMAGE_O1[:6]),
            # The report: the overlays noted, nothing unreached (the word after
            # the %%ovcall is reached, helper is entered from ov1), and the
            # %%ovcall's words a span of their own.
            "optgrep|-O1|^This program has drum overlays",
            "nooptgrep|-O1|^Apparently unreached",
            "optgrep|-O1|^  span 1 .* line 13, %%ovcall$"]


F10_VARIANTS = [
    # A pinned word: ov1 names pinA, the cla T6 deletes in the control.
    ("f10_pinword", [("        jsp helper\n", "        jsp helper\n        lac pinA\n")],
        ["okgrep|-O1|^000102 760200$", "optgrep|-O1|Per reason: P2-taken 1$"]),
    # An offset: ov1 names pinA+2, the sza; the cla between is held as well.
    ("f10_pinoffset", [("        jsp helper\n", "        jsp helper\n        lac pinA+2\n")],
        ["okgrep|-O1|^000102 760200$", "okgrep|-O1|^000104 640100$"]),
    # ov1 no longer calls helper: nothing enters it, and it is listed.
    ("f10_noentry", [("        jsp helper\n", "")],
        ["optgrep|-O1|first label helper"]),
    # ovEnter in bank 1: the %%ovcall is eem, jsp i [ovEnter:1] and the data
    # words.  After -O1's three deletions rootVal is at 0114, rootFwd 0115,
    # ovTable 0116, and bank 0's pool, the literal, at 0124.
    ("f10_xbank", [("bank 0\n100/\novEnter, jmp .\ngo,", "bank 1\n100/\novEnter, jmp .\nbank 0\n100/\ngo,")],
        ["ok|-O1|",
         "okgrep|-O1|^000104 724074$",
         "okgrep|-O1|^000105 630124$",
         "okgrep|-O1|^000106 000120$",
         "okgrep|-O1|^000107 001002$",
         "okgrep|-O1|^000124 010100$",
         "nooptgrep|-O1|^Apparently unreached"]),
]

# F10's window: mk is marked %%inline and called twice; each copy spends two
# words, so the root's last word, z at 0112, would move to 0116.  In
# f10_window, w begins at 0113, just past z, and relayout refuses both copies;
# in f10_windowhigh it begins at 0200 and both are made.
F10_WINDOW = """\
TITLE
bank 0
%%inline mk
100/
main,   jsp mk
        jsp mk
        jmp .
mk,     dap mkrt
        lac x
        add y
        dac z
mkrt,   jmp 0
x,      3
y,      4
z,      0
%%window w, FIRST, 1777
%%overlay ov1, w
        export ov1Go
ov1Go,  jmp .
%%endoverlay
2000/
%%ovtable
        start main
"""

F10_WINDOWS = [
    ("f10_window", "f10_window: an inline would push the root into a window", "113",
        ["warn|-O1|inline: relayout refused the copy at bank 0 0100 (window)",
         "warn|-O1|inline: relayout refused the copy at bank 0 0101 (window)",
         "okgrep|-O1|^000100 620103$",
         "okgrep|-O1|^000112 000000$"]),
    ("f10_windowhigh", "f10_windowhigh: f10_window with the window higher", "200",
        ["nowarn|-O1|(window)",
         "optgrep|-O1|0 copies were refused by relayout",
         "okgrep|-O1|^000116 000000$"]),
]


# F11: stage B, each overlay optimized in a view of its own, the root's words
# as context.  f11_twins holds two overlays of window w, each with a region:
#   ov1: 01002 ov1Go dap ov1X  01003 sza  01004 jmp ov1A (T3: jmp ov1X)
#        01005 dac ov1V  01006 cma  01007 ov1A jmp ov1X  01010 ov1X jmp .
#        01011 ov1V
#   ov2: 01002 ov2V  01003 ov2Go dap ov2X  01004 lac [5] (T8d: law 5)
#        01005 add [5]  01006 dac ov2V  01007 ov2X jmp .  01010 the literal
# Each overlay writes a word at the address the other's rewrite reads: ov2
# writes 01007, where ov1's T3 reads through ov1A, and ov1 writes 01010, ov2's
# pool word.  Seen together, as by -M, both rewrites would be refused.  The
# second add [5] keeps the pool word, so no length changes.  ov1's sza is at
# ov2's entry address, and ov2V at ov1's: neither is an entry in its own view.
F11 = """\
f11_twins: two overlays of one window, each rewritten in its own view
bank 0
100/
ovEnter, jmp .
go,     cla
        %%ovcall ov1, ov1Go
        %%ovcall ov2, ov2Go
        hlt
%%window w, 1000, 1777
%%overlay ov1, w
        export ov1Go
ov1Go,  dap ov1X
        sza
%%optimize
        jmp ov1A
%%endoptimize
        dac ov1V
        cma
ov1A,   jmp ov1X
ov1X,   jmp .
ov1V,   0
%%endoverlay
%%overlay ov2, w
        export ov2Go
ov2V,   0
ov2Go,  dap ov2X
%%optimize
        lac [5]
%%endoptimize
        add [5]
        dac ov2V
ov2X,   jmp .
%%endoverlay
%%ovtable
        start go
"""

# The overlays' words from 01002, as written; F11_O1 the words -O1 and -O2
# rewrite, by hand.
F11_WORDS = [("ov1", [0o261010, 0o640100, 0o601007, 0o241011, 0o761000, 0o601010, 0o601010, 0o000000]),
             ("ov2", [0o000000, 0o261007, 0o201010, 0o401010, 0o241002, 0o601007, 0o000005])]
F11_O1 = {("ov1", 0o1004): 0o601010, ("ov2", 0o1004): 0o700005}


def f11_lines():
    """f11_twins' lines: every overlay word without -O, at -O1 and at -O2."""
    lines = ["ok||"]
    for flags in ("", "-O1", "-O2"):
        for name, words in F11_WORDS:
            image = []
            for i, word in enumerate(words):
                addr = 0o1002 + i
                if flags:
                    word = F11_O1.get((name, addr), word)
                image.append(word)
                lines.append("okgrep|%s|^%06o %06o %s$" % (flags, addr, word, name))
            # The .ovl's first line of the overlay: the stamp, its number and
            # its first six words.
            lines.append("ovlgrep|bin %s|^[0-7]{6} %06o %s$"
                          % (flags, int(name[2]), " ".join("%06o" % w for w in image[:6])))
    lines += [
        # BV5: an export reached only by %%ovcall is not unreached.  Its
        # -O=flow half, which needs am1test, is Tests/Optimizer/b5_entries.sh.
        "nooptgrep|-O1|^Apparently unreached",
        # Each overlay's section reports its own rewrite.
        "optgrep|-O1|^    T3   bank  0 1004@ov1  .*f11_twins.am1:15  region [0-9]+$",
        "optgrep|-O1|^    T8d  bank  0 1004@ov2  .*f11_twins.am1:28  region [0-9]+$"]
    return lines


# B1: a region, span or speed region open at an overlay's boundary is an
# error, with or without -O.
F11_VARIANTS = [
    ("f11_crossend", [("%%endoptimize\n        dac ov1V\n", "        dac ov1V\n"),
                      ("ov1V,   0\n%%endoverlay\n", "ov1V,   0\n%%endoverlay\n%%endoptimize\n")],
        ["fail||%%endoverlay of ov1 is inside the optimize region opened at line 14",
         "fail|-O1|%%endoverlay of ov1 is inside the optimize region opened at line 14"]),
    ("f11_crossopen", [("%%overlay ov2, w\n", "%%nooptimize\n%%overlay ov2, w\n"),
                       ("ov2X,   jmp .\n", "ov2X,   jmp .\n%%endnooptimize\n")],
        ["fail||%%overlay ov2 is inside the nooptimize span opened at line 23",
         "fail|-O1|%%overlay ov2 is inside the nooptimize span opened at line 23"]),
    ("f11_crossspeed", [("%%overlay ov1, w\n", "%%speed\n%%overlay ov1, w\n"),
                        ("        export ov1Go\n", "        export ov1Go\n%%endspeed\n")],
        ["fail||%%overlay ov1 is inside the speed region opened at line 10",
         "fail|-O1|%%overlay ov1 is inside the speed region opened at line 10"]),
]

# f11_pins: overlay ovj patches the root's rootX, a jmp rootY that overlay
# ovk jumps to.  In ovk's view rootX keeps ovj's pin, so ovk's T3 through it,
# jmp rootX to jmp rootY, is refused for P2.  Root: 0100 ovEnter, 0101 go cla,
# 0102-0107 the two %%ovcalls, 0110 hlt, 0111 rootX, 0112 rootY.  ovk: 01002
# dap ovkX, 01003 sza, 01004 jmp rootX, 01005 ovkX jmp .
F11_PINS = """\
f11_pins: overlay ovj patches the root word overlay ovk jumps through
bank 0
100/
ovEnter, jmp .
go,     cla
        %%ovcall ovj, ovjGo
        %%ovcall ovk, ovkGo
        hlt
rootX,  jmp rootY
rootY,  hlt
%%window w, 1000, 1777
%%overlay ovj, w
        export ovjGo
ovjGo,  dap ovjX
        dap rootX
ovjX,   jmp .
%%endoverlay
%%overlay ovk, w
        export ovkGo
ovkGo,  dap ovkX
        sza
%%optimize
        jmp rootX
%%endoptimize
ovkX,   jmp .
%%endoverlay
%%ovtable
        start go
"""

F11_PINS_LINES = ["ok||",
                  "okgrep||^001004 600111 ovk$",
                  "okgrep|-O1|^001004 600111 ovk$",
                  "okgrep|-O1|^000111 600112$",
                  # ovk's T3 refused by P2 on the word it reads through,
                  # through-taken, in ovk's section: the root's own T13 at
                  # rootX is refused for P2 as well, so "Per reason" won't do.
                  "optgrep|-O1|^    bank  0 1004  .*f11_pins.am1:23  the address of the word the rule reads through is used as a value"]

# The control: without ovj's dap, ovk's T3 is taken.
F11_PINS_VARIANTS = [
    ("f11_nopin", [("        dap rootX\n", "")],
        ["okgrep||^001004 600111 ovk$",
         "okgrep|-O1|^001004 600112 ovk$",
         "nooptgrep|-O1|the rule reads through is used as a value"]),
]


# f11_shrink: BV6, a deletion in an overlay.  ov1's T6 deletes the cla at
# 01003, so the overlay's words from 01004 move down one, its length and the
# table's word for it with them, the stamp changes, and %%ovcall's data word
# for ov1B (root 0107) follows the entry from 01006 to 01005; no root word
# moves.  hand/f11_shrink.am1 is the deletion made by hand, an empty line in
# the cla's place so that the line numbers in the .osym agree.  The root's
# helper, entered only from ov1, keeps ov1's pin: its own T6 is refused P2
# in the root's view, and is BV9's bait (Tests/Optimizer/b9_control.sh).
F11_SHRINK = """\
f11_shrink: a deletion in an overlay relays the overlay alone
bank 0
100/
ovEnter, jmp .
go,     cla
        %%ovcall ov1, ov1Go
        %%ovcall ov1, ov1B
        hlt
%%optimize
helper, cla
        lac rootV
%%endoptimize
        hlt
rootV,  5
%%window w, 1000, 1777
%%overlay ov1, w, persistent
        export ov1Go
        export ov1B
ov1Go,  dap ov1X
%%optimize
        cla
        lac ov1V
%%endoptimize
        dac ov1V
        var cnt
ov1B,   idx cnt
        add [ov1V]
        sza
        jmp helper
ov1X,   jmp .
ov1V,   0
%%endoverlay
%%ovtable
        start go
"""

F11_SHRINK_LINES = ["ok||",
                    # BV6: the -O1 build's files are the hand build's; the
                    # build without -O is not, or the check could not fail.
                    "hand|bin -s -O1|rim",
                    "hand|bin -s -O1|ovl",
                    "hand|bin -s -O1|osym",
                    "nohand|bin -s|rim",
                    "nohand|bin -s|ovl",
                    "nohand|bin -s|osym",
                    "okgrep|-O1|^000107 001005$",
                    # lac ov1V, moved into the cla's place, and [ov1V], each
                    # naming ov1V at its new address
                    "okgrep|-O1|^001003 201012 ov1$",
                    "okgrep|-O1|^001013 001012 ov1$",
                    "optgrep|-O1|^    T6   bank  0 1003@ov1  .*f11_shrink.am1:21  deleted 1003: cla => \\(deleted\\)$",
                    "optgrep|-O1|^    bank  0 0111  .*f11_shrink.am1:10  P2: "]


# f11_full: BV8, the window is an overlay's ceiling.  ceil_unroll's loop in
# ov1's speed region: 01002 dap ov1X, 01003 law i 3, 01004 dac ca, 01005-01007
# the body, 01010 isp ca, 01011 jmp ha, 01012 cla, 01013 ov1X, 01014 ca,
# 01015 na, 01016 one.  Unrolled it is 2 words longer, and window w ends at
# 01017, 1 word free: the budget refuses it.  The control, f11_roomy, ends the
# window at 01020, exactly 2 free: it is unrolled, though its copies land
# before its setup is deleted, and one ends at 01020.  -O=inline=reserve:0,
# since the unroll room keeps the inline reserve back too.
F11_FULL = """\
f11_full: an unroll in an overlay that would pass its window's last address
bank 0
100/
ovEnter, jmp .
go,     cla
        %%ovcall ov1, ov1Go
        hlt
%%window w, 1000, 1017
%%overlay ov1, w
        export ov1Go
ov1Go,  dap ov1X
%%speed
        law i 3
        dac ca
ha,     lac na
        add one
        dac na
        isp ca
        jmp ha
%%endspeed
        cla
ov1X,   jmp .
ca,     0
na,     0
one,    1
%%endoverlay
%%ovtable
        start go
"""

F11_FULL_LINES = ["ok||",
                  "optgrep|-O1 -O=inline=reserve:0|^    S2   bank  0 1005@ov1  .*f11_full.am1:18  ha  not unrolled: window w has too few free words for 2 more$",
                  "okgrep|-O1 -O=inline=reserve:0|^001016 000001 ov1$"]

F11_FULL_VARIANTS = [
    ("f11_roomy", [("%%window w, 1000, 1017\n", "%%window w, 1000, 1020\n")],
        ["optgrep|-O1 -O=inline=reserve:0|^    S2   bank  0 1005@ov1  .*f11_roomy.am1:18  ha  unrolled, 3 trips of 3 words, length \\+2, saves 55 us$",
         "okgrep|-O1 -O=inline=reserve:0|^001020 000001 ov1$",
         "okgrep|-O1 -O=inline=reserve:0|^001003 201017 ov1$",
         "okgrep||^001016 000001 ov1$"]),
]


def write(name, text):
    with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="") as f:
        f.write(text)


def family(base, variants, control, manifest, names):
    """Write a control and its one-change variants, adding their MANIFEST lines."""
    cname = base.split(":", 1)[0]
    write(cname + ".am1", base)
    for line in control:
        manifest.append(cname + "|" + line)

    for name, changes, lines in variants:
        assert name not in names, name
        names.append(name)
        text = base
        for old, new in changes:
            assert text.count(old) == 1, (name, old)
            text = text.replace(old, new)
        # The first line is the program's title.
        title = name + ": " + base.split("\n", 1)[0].split(": ", 1)[1]
        text = title + "\n" + text.split("\n", 1)[1]
        write(name + ".am1", text)
        for line in lines:
            manifest.append(name + "|" + line)


def main():
    names = []
    manifest = ["# name|expect|flags|text -- written by make_fixtures.py; see run_tests.sh"]

    write("f1_endoverlay.ah", INCLUDE)
    family(BASE, VARIANTS, CONTROL, manifest, names)
    family(F2_BASE, F2_VARIANTS, F2_CONTROL, manifest, names)

    write("f3_ovcall.am1", F3)
    write("f3_ovcall.ref", F3_REF)
    for line in F3_LINES:
        manifest.append("f3_ovcall|" + line)

    write("f4_layout.am1", F4)
    for line in F4_LINES:
        manifest.append("f4_layout|" + line)
    for name, count, lines in F4_MANY:
        write(name + ".am1", many_overlays(name, count))
        for line in lines:
            manifest.append(name + "|" + line)

    # F5: the base and its variants, each .ref worked out by f5_ref(); the
    # three stamps must differ, or the variants prove nothing.
    stamps = {}
    for name, changes in [("f5_table", [])] + F5_VARIANTS:
        text = F5
        overlays = [list(ov) for ov in F5_OVERLAYS]
        for old, new in changes:
            assert text.count(old) == 1, (name, old)
            text = text.replace(old, new)
        if name == "f5_word":
            overlays[0][4] = [0o760400, 0o124]
        title = name + ": " + F5.split("\n", 1)[0].split(": ", 1)[1]
        text = title + "\n" + text.split("\n", 1)[1]
        stamps[name], ref = f5_ref([tuple(ov) for ov in overlays], F5_TRACKS[name])
        write(name + ".am1", text)
        write(name + ".ref", ref)
        manifest.append(name + "|refsub||")
    assert len(set(stamps.values())) == len(stamps), stamps

    # F6: the files, from the F5 program under its own name.
    text = "f6_files: the .ovl, .osym and .sym files, worked out by hand\n" + F5.split("\n", 1)[1]
    write("f6_files.am1", text)
    write("f6_files.ovl.ref", f6_ovl("f6_files.am1", F5_OVERLAYS, F5_TRACKS["f5_table"]))
    write("f6_files.osym.ref", f6_osym(text, "f6_files.am1"))
    write("f6_files.sym.ref", f6_sym(text, "f6_files.am1"))
    for ext in ("ovl", "osym", "sym"):
        manifest.append("f6_files|same|bin -s|" + ext)

    # F7: the defines, their control, and the manager.
    write("f7_defines.am1", f7_source(
        "f7_defines: overlay.ah's defines beside the table and headers am1 makes", []))
    write("f7_swapped.am1", f7_source(
        "f7_swapped: f7_defines with two entry offsets and the header offsets swapped", F7_SWAPS))
    for pair in F7_PAIRS:
        manifest.append("f7_defines|pairs||" + pair)
        manifest.append("f7_swapped|nopairs||" + pair)
    write("f7_manager.am1", F7_MANAGER)
    manifest.append("f7_manager|ok||")
    manifest.append("f7_manager|ok|bin|")

    # F9: -S's figure per bank.
    for name, text, lines in F9:
        write(name + ".am1", text)
        for line in lines:
            manifest.append(name + "|" + line)

    # F10: the optimizer.
    family(F10, F10_VARIANTS, f10_lines(), manifest, names)
    for name, title, first, lines in F10_WINDOWS:
        write(name + ".am1", F10_WINDOW.replace("TITLE", title).replace("FIRST", first))
        for line in lines:
            manifest.append(name + "|" + line)

    # F11: each overlay in a view of its own.
    family(F11, F11_VARIANTS, f11_lines(), manifest, names)
    family(F11_PINS, F11_PINS_VARIANTS, F11_PINS_LINES, manifest, names)
    write("f11_shrink.am1", F11_SHRINK)
    os.makedirs(os.path.join(HERE, "hand"), exist_ok=True)
    old = "%%optimize\n        cla\n        lac ov1V\n"
    assert F11_SHRINK.count(old) == 1
    write(os.path.join("hand", "f11_shrink.am1"),
          F11_SHRINK.replace(old, "%%optimize\n\n        lac ov1V\n"))
    for line in F11_SHRINK_LINES:
        manifest.append("f11_shrink|" + line)
    family(F11_FULL, F11_FULL_VARIANTS, F11_FULL_LINES, manifest, names)

    write("MANIFEST", "\n".join(manifest) + "\n")

    print("wrote f1_valid.am1, f2_valid.am1, f1_endoverlay.ah, %d variants, f3_ovcall.am1 and .ref, "
          "f4_layout, f4_fit, f4_full, f5_table, f5_word and f5_track with .ref, f6_files with "
          "three .ref, f7_defines, f7_swapped, f7_manager, %s, f10_pins, f10_window, "
          "f10_windowhigh, f11_twins, f11_pins, f11_shrink and its hand build, f11_full, MANIFEST"
          % (len(names), ", ".join(name for name, _, _ in F9)))
    print("F5 stamps: " + ", ".join("%s %06o" % (k, v) for k, v in sorted(stamps.items())))


if __name__ == "__main__":
    main()

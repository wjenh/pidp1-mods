#!/bin/sh
# roundtrip.sh -- tests mkmicrotape: blank, check, deferred formatting (short images), and the
# simh 18b import/export round trip.
#
# Usage: sh roundtrip.sh ./mkmicrotape      (make roundtrip does this)
#
# Needs only POSIX sh, dd, od, awk, cmp and /dev/urandom. od reads words in host order, and
# simh files are little-endian, so this assumes a little-endian host (x86, the Pi's ARM).
# Leaves nothing behind: every rt-* file is removed at the end.
#
# 10-Sep-2026 Claude -- initial version, for the Type 550 task (Magtape/TASK-TYPE550.md).
# 11-Sep-2026 Claude -- deferred formatting: blank is empty, import trims blank blocks, short
#                       images check and export (Magtape/TASK-REWORK.md).

MK="$1"
[ -x "$MK" ] || { echo "usage: sh roundtrip.sh <path to mkmicrotape>"; exit 2; }

FAILS=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; FAILS=`expr $FAILS + 1`; }

BLKBYTES=1032           # 258 words x 4
IMGBYTES=594432         # 576 blocks x 258 words x 4
SIMHWORDS=147968        # 578 blocks x 256 words
DATAWORDS=147456        # 576 blocks x 256 words

rm -f rt-*

# Blank: an empty file (deferred formatting), every block checks, and the output is not
# overwritten without -f.
"$MK" blank rt-blank.img && [ -f rt-blank.img ] && [ `wc -c < rt-blank.img` -eq 0 ] \
    && pass "blank image is an empty file" || fail "blank image is an empty file"
"$MK" check rt-blank.img > rt-check.out && grep -q "576 blocks, 0 in the file, 0 fail" rt-check.out \
    && pass "blank image checks" || fail "blank image checks"
"$MK" blank rt-blank.img 2> /dev/null; [ $? -eq 2 ] && pass "existing output refused without -f" || fail "existing output refused without -f"
"$MK" -f blank rt-blank.img && pass "-f overwrites" || fail "-f overwrites"

# The blank image exports as a full simh tape of zeros.
"$MK" export rt-blank.img rt-blank.dt 2> /dev/null && [ `wc -c < rt-blank.dt` -eq `expr $SIMHWORDS \* 4` ] \
    && od -An -tu4 -v rt-blank.dt | tr -s ' ' '\n' | grep -v '^$' | awk '$1 != 0 { bad++ } END { exit (bad > 0) }' \
    && pass "blank image exports as 578 zero blocks" || fail "blank image exports as 578 zero blocks"

# A full simh file of random words, including blocks 576-577: import warns about those blocks,
# masks every word to 18 bits, and the result checks. The last block is not blank, so the
# image is full size.
dd if=/dev/urandom of=rt-src.dt bs=1024 count=578 2> /dev/null
"$MK" import rt-src.dt rt-a.img 2> rt-import.err
[ $? -eq 0 ] && pass "import" || fail "import"
grep -q "blocks 576-577" rt-import.err && pass "import warns about blocks 576-577" || fail "import warns about blocks 576-577"
[ `wc -c < rt-a.img` -eq $IMGBYTES ] && pass "a full import is full size" || fail "a full import is full size"
"$MK" check rt-a.img > /dev/null && pass "imported image checks" || fail "imported image checks"

# Export: word i of the simh file is word i of the source masked to 18 bits; blocks 576-577 zero.
"$MK" export rt-a.img rt-b.dt 2> /dev/null && [ `wc -c < rt-b.dt` -eq `expr $SIMHWORDS \* 4` ] \
    && pass "export size" || fail "export size"
od -An -tu4 -v rt-src.dt | tr -s ' ' '\n' | grep -v '^$' > rt-src.txt
od -An -tu4 -v rt-b.dt | tr -s ' ' '\n' | grep -v '^$' > rt-b.txt
paste rt-src.txt rt-b.txt | awk -v n=$DATAWORDS '
    { want = (NR <= n) ? ($1 % 262144) : 0; if ($2 != want) { bad++ } }
    END { exit (bad > 0 || NR != 147968) }' \
    && pass "exported words are the imported ones, masked; blocks 576-577 zero" \
    || fail "exported words are the imported ones, masked; blocks 576-577 zero"

# Round trip: importing the export gives back the identical image, with no warning.
"$MK" import rt-b.dt rt-c.img 2> rt-import2.err && cmp -s rt-a.img rt-c.img \
    && pass "import(export(image)) is identical" || fail "import(export(image)) is identical"
[ -s rt-import2.err ] && fail "second import printed a warning" || pass "second import is silent"

# A short simh file (10 blocks of data): the missing words are zero, with a note, and the
# image holds only the 10 blocks -- the blank rest is left off the file.
dd if=rt-b.dt of=rt-short.dt bs=1024 count=10 2> /dev/null
"$MK" import rt-short.dt rt-d.img 2> rt-short.err && grep -q "the rest are zero" rt-short.err \
    && "$MK" check rt-d.img > /dev/null && pass "short simh file imports, zero-filled" || fail "short simh file imports, zero-filled"
[ `wc -c < rt-d.img` -eq `expr 10 \* $BLKBYTES` ] && pass "short import leaves the blank blocks off the file" \
    || fail "short import leaves the blank blocks off the file"

# A short image exports in full: its 10 blocks' data, then zeros.
"$MK" export rt-d.img rt-g.dt 2> /dev/null && [ `wc -c < rt-g.dt` -eq `expr $SIMHWORDS \* 4` ] \
    && od -An -tu4 -v rt-short.dt | tr -s ' ' '\n' | grep -v '^$' > rt-short.txt \
    && od -An -tu4 -v rt-g.dt | tr -s ' ' '\n' | grep -v '^$' > rt-g.txt \
    && awk 'NR == FNR { src[NR] = $1; n = NR; next }
            { want = (FNR <= n) ? src[FNR] : 0; if ($1 != want) { bad++ } }
            END { exit (bad > 0 || FNR != 147968) }' rt-short.txt rt-g.txt \
    && pass "short image exports its blocks, then zeros" || fail "short image exports its blocks, then zeros"

# A damaged word: check names exactly that block and exits 1.
cp rt-a.img rt-e.img
printf '\001\000\000\000' | dd of=rt-e.img bs=4 seek=`expr 7 \* 258 + 100` conv=notrunc 2> /dev/null
"$MK" check rt-e.img > rt-check.out
[ $? -eq 1 ] && grep -q "^block 0007:" rt-check.out && grep -q "1 fail" rt-check.out \
    && pass "check finds the one damaged block" || fail "check finds the one damaged block"

# A file that is not a whole number of blocks, or longer than a tape, is refused.
dd if=rt-a.img of=rt-f.img bs=4 count=1000 2> /dev/null
"$MK" check rt-f.img > /dev/null 2>&1; [ $? -eq 2 ] && pass "part-block image refused" || fail "part-block image refused"
dd if=rt-a.img of=rt-h.img bs=`expr $BLKBYTES + 2` count=1 2> /dev/null
"$MK" check rt-h.img > /dev/null 2>&1; [ $? -eq 2 ] && pass "block plus two bytes refused" || fail "block plus two bytes refused"
cat rt-a.img rt-d.img > rt-i.img
"$MK" check rt-i.img > /dev/null 2>&1; [ $? -eq 2 ] && pass "oversize image refused" || fail "oversize image refused"

rm -f rt-*
echo "roundtrip: $FAILS failed"
[ $FAILS -eq 0 ]

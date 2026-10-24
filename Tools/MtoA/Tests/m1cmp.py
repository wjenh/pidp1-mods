#!/usr/bin/env python3
# Compare what macro1 loads with what am1 loads for the same program.
#
# Usage: m1cmp.py file.rim file.lst file.dmp
#   file.rim  macro1's tape, assembled with -r (plain RIM: dio/word pairs, then jmp start)
#   file.lst  macro1's listing of the same assembly, for the words of each constants pool
#   file.dmp  am1's -T output for the converted source
#
# Every word must be identical, at the same address, and so must the start address (a tape
# with no start matches am1's stop, which -T writes as 0), except
# inside constants pools: am1 lays a pool out in its own order, so a pool's words may be in any
# order, and a word that refers to a pool slot may refer to another slot holding the same value.
# A pool is a run of words from the address of a "constants" statement; am1's run may be longer
# than macro1's (am1 may give equal values separate slots) but only into addresses macro1 leaves
# unloaded.
#
# Exit 0 when they agree, 1 when they differ (each difference is printed), 2 on a bad input.
# "-" as the .dmp means am1 produced nothing, which is always a difference.

import re
import sys

ADDR = 0o7777


def rim_words(path):
    data = open(path, "rb").read()
    words, cur, n = [], 0, 0
    for b in data:
        # Leader and blank tape have channel 8 clear; every data frame has it set.
        if b & 0o200:
            cur = (cur << 6) | (b & 0o77)
            n += 1
            if n == 3:
                words.append(cur)
                cur, n = 0, 0
    mem, start, i = {}, None, 0
    while i < len(words):
        w = words[i]
        # macro1 addresses are 12 bits; the rest of the word is the opcode.
        if (w & 0o760000) == 0o320000 and i + 1 < len(words):
            mem[w & ADDR] = words[i + 1]
            i += 2
        elif (w & 0o760000) == 0o600000:
            start = w & ADDR
            i += 1
        else:
            raise SystemExit("m1cmp: %s: word %06o at %d is neither dio nor jmp" % (path, w, i))
    return mem, start


def dmp_words(path):
    mem, start = {}, None
    lines = [l.split() for l in open(path).read().split("\n") if l.strip()]
    if not lines:
        return mem, start
    # The last line is the start (or stop) statement's value, not a storage word.
    for f in lines[:-1]:
        if len(f) != 2:
            raise SystemExit("m1cmp: %s: overlay or odd line %s" % (path, " ".join(f)))
        mem[int(f[0], 8)] = int(f[1], 8)
    last = lines[-1]
    start = int(last[1], 8)
    return mem, start


LISTLINE = re.compile(r"^\s*(\d+) (\d{5}) (\d{6})      (.*)$")
CONTLINE = re.compile(r"^      (\d{5}) (\d{6})$")
CONSTSTMT = re.compile(r"(^|[\t ,])consta[a-z]*\s*$")


def pool_starts(path):
    # A constants statement with any literals is listed with the first pool word, and the
    # other pool words follow as continuation lines.
    starts = set()
    for line in open(path, errors="replace").read().split("\n"):
        m = LISTLINE.match(line)
        if m:
            stmt = m.group(4).split("/")[0]
            if CONSTSTMT.search(stmt.rstrip()):
                starts.add(int(m.group(2), 8))
    return starts


def main():
    if len(sys.argv) != 4:
        raise SystemExit(2)
    m1, m1start = rim_words(sys.argv[1])
    starts = pool_starts(sys.argv[2])
    if sys.argv[3] == "-":
        print("am1 produced no words")
        return 1
    a1, a1start = dmp_words(sys.argv[3])

    # macro1's pool slots: the contiguous loaded words from each pool start.
    m1pool = {}
    for s in sorted(starts):
        a = s
        while a in m1:
            m1pool[a] = s
            a += 1
            if a in starts:
                break
    # am1's pool slots: from the same start, over words that are macro1 pool words or that
    # macro1 does not load at all.
    a1pool = {}
    for s in sorted(starts):
        a = s
        while a in a1 and (m1pool.get(a) == s or a not in m1):
            a1pool[a] = s
            a += 1
            if a in starts:
                break

    errors = []
    if m1start != a1start and not (m1start is None and a1start == 0):
        errors.append("start: macro1 %s, am1 %s" % (
            "none" if m1start is None else "%06o" % m1start,
            "none" if a1start is None else "%06o" % a1start))

    # Match each macro1 pool slot with the am1 slots of the same pool holding the same
    # constant. A slot matches one holding the same word; failing that, a slot whose word names
    # another pool slot (a constant of a constant) matches one naming a match of that slot with
    # the same high bits. The same word is tried first, so a number that falls in a pool's
    # range is never read as an address when it need not be.
    phi = {}
    byval = {}
    for a in a1pool:
        byval.setdefault((a1pool[a], a1[a]), set()).add(a)
    for m in m1pool:
        if (m1pool[m], m1[m]) in byval:
            phi[m] = byval[(m1pool[m], m1[m])]
    grew = True
    while grew:
        grew = False
        for m in m1pool:
            low = m1[m] & ADDR
            if m in phi or low not in phi:
                continue
            hit = set()
            for t in phi[low]:
                hit |= byval.get((m1pool[m], (m1[m] & ~ADDR) | t), set())
            if hit:
                phi[m] = hit
                grew = True

    for s in sorted(starts):
        unmatched = [m for m in m1pool if m1pool[m] == s and m not in phi]
        used = set()
        for m in m1pool:
            if m1pool[m] == s and m in phi:
                used |= phi[m]
        extra = [a for a in a1pool if a1pool[a] == s and a not in used]
        if unmatched or extra:
            errors.append("pool at %06o: macro1 words with no match %s; am1 words with none %s" % (
                s, " ".join("%06o" % m1[m] for m in sorted(unmatched)[:6]),
                " ".join("%06o" % a1[a] for a in sorted(extra)[:6])))

    for a in sorted(set(m1) | set(a1)):
        if a in m1pool or a in a1pool:
            continue
        if a not in m1:
            errors.append("%06o: am1 loads %06o, macro1 loads nothing" % (a, a1[a]))
        elif a not in a1:
            errors.append("%06o: macro1 loads %06o, am1 loads nothing" % (a, m1[a]))
        elif m1[a] != a1[a]:
            # A differing pair may differ only by which slot of a pool it names.
            low = m1[a] & ADDR
            if not (low in phi and (a1[a] & ~ADDR) == (m1[a] & ~ADDR) and (a1[a] & ADDR) in phi[low]):
                errors.append("%06o: macro1 %06o, am1 %06o" % (a, m1[a], a1[a]))

    for e in errors[:40]:
        print(e)
    if len(errors) > 40:
        print("... %d differences in all" % len(errors))
    if not errors:
        print("words agree: %d words, %d pool words, start %s" % (
            len(m1), len(m1pool), "none" if m1start is None else "%06o" % m1start))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

#!/bin/bash
# Test kit for mactoam1: convert macro1 sources and check that am1 loads macro1's own words.
#
# Usage: run_tests.sh [-m mactoam1] [-a am1] [-w workdir] [t1|t2|t3|control|all]...
#   -m  the converter to test; default: built from this checkout's Tools/MtoA into the work dir
#   -a  the am1 to use; default /opt/pidp1-mods/bin/am1
#   -w  work directory; default ./mtoatest under the current directory. Everything the run
#       writes goes there; the checkout is only read.
#   t1       the corpus, corpus.txt
#   t2       the construct fixtures, fixtures/*.mac
#   t3       the refusal fixtures, refuse/*.mac
#   control  the control legs: the old converter (built from git history) must fail where section 2
#            of the task says, and a word changed by hand, in code and in a constants pool,
#            must fail the compare
#   all      t1 t2 t3 control (the default)
#
# Needs gcc, python3 and, for control, flex and git. Exit status 0 when every check passes.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
MTOA=""
AM1=/opt/pidp1-mods/bin/am1
WORK=$PWD/mtoatest
while getopts "m:a:w:" opt; do
    case $opt in
    m) MTOA=$(readlink -f "$OPTARG") ;;
    a) AM1=$(readlink -f "$OPTARG") ;;
    w) WORK=$OPTARG ;;
    *) sed -n '3,18p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
TARGETS=${*:-all}
[ "$TARGETS" = all ] && TARGETS="t1 t2 t3 control"

mkdir -p "$WORK/build" "$WORK/t1" "$WORK/t2" "$WORK/t3" "$WORK/control"
WORK=$(cd "$WORK" && pwd)
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf "  pass  %s\n" "$*"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL  %s\n" "$*"; }

# macro1, the oracle, from this checkout.
gcc -O1 -w -o "$WORK/build/macro1" "$ROOT/src/macro/macro1_1.c" || { echo "macro1 build failed"; exit 2; }
M1=$WORK/build/macro1

if [ -z "$MTOA" ]; then
    rm -rf "$WORK/build/src"
    mkdir -p "$WORK/build/src"
    cp "$ROOT"/Tools/MtoA/*.c "$ROOT"/Tools/MtoA/*.h "$ROOT"/Tools/MtoA/Makefile "$WORK/build/src/" 2>/dev/null
    ( cd "$WORK/build/src" && make -s mactoam1 ) >"$WORK/build/make.log" 2>&1 || { cat "$WORK/build/make.log"; echo "mactoam1 build failed"; exit 2; }
    MTOA=$WORK/build/src/mactoam1
fi
echo "macro1 $(md5sum < "$M1" | cut -c1-8), mactoam1 $(md5sum < "$MTOA" | cut -c1-8) ($MTOA), am1 $(md5sum < "$AM1" | cut -c1-8)"

# check DIR NAME SRC [lf] [title]
# Assemble SRC with macro1, convert it, assemble the conversion with am1 -T, compare.
# lf: macro1 gets a copy with CR-only line ends made LF (the converter takes the original).
# title: both get a copy with a title line added in front.
# Returns 0 when the words agree; leaves NAME.* files in DIR.
check() {
    local dir=$1 name=$2 src=$3 lf=${4:-} title=${5:-}
    ( cd "$dir" && rm -f "$name".* )
    if [ -n "$title" ]; then
        { echo "$name, title added by the test kit"; cat "$src"; } > "$dir/$name.conv.mac"
    else
        cp "$src" "$dir/$name.conv.mac"
    fi
    if [ -n "$lf" ]; then
        tr '\r' '\n' < "$dir/$name.conv.mac" > "$dir/$name.mac"
    else
        cp "$dir/$name.conv.mac" "$dir/$name.mac"
    fi
    ( cd "$dir" && timeout 30 "$M1" -r "$name.mac" > "$name.m1.log" 2>&1 )
    if [ ! -f "$dir/$name.rim" ]; then
        echo "macro1 did not assemble it" > "$dir/$name.why"
        return 2
    fi
    timeout 30 "$MTOA" -o "$dir/$name.am1" "$dir/$name.conv.mac" > "$dir/$name.mtoa.log" 2>&1
    echo $? > "$dir/$name.mtoa.rc"
    ( cd "$dir" && timeout 60 "$AM1" -T "$name.am1" > "$name.am1.log" 2>&1 )
    if [ ! -f "$dir/$name.dmp" ]; then
        { echo "am1 failed:"; head -3 "$dir/$name.am1.log"; } > "$dir/$name.why"
        return 1
    fi
    python3 -B "$HERE/m1cmp.py" "$dir/$name.rim" "$dir/$name.lst" "$dir/$name.dmp" > "$dir/$name.why" 2>&1
}

# refusal DIR NAME SRC
# The converter must exit nonzero, name a line on stderr, and write output am1 refuses.
# Sets RLINE to the line number named, or empty.
refusal() {
    local dir=$1 name=$2 src=$3 rc
    rm -f "$dir/$name.am1" "$dir/$name.dmp"
    timeout 30 "$MTOA" -o "$dir/$name.am1" "$src" > "$dir/$name.mtoa.log" 2>&1
    rc=$?
    RLINE=$(sed -n 's/^[^:]*:\([0-9][0-9]*\): error:.*/\1/p' "$dir/$name.mtoa.log" | head -1)
    [ $rc -ne 0 ] || return 1
    [ -n "$RLINE" ] || return 1
    ( cd "$dir" && timeout 60 "$AM1" -T "$name.am1" > "$name.am1.log" 2>&1 )
    [ ! -f "$dir/$name.dmp" ] || return 1
    grep -q 'mactoam1' "$dir/$name.am1.log" || return 1
    return 0
}

t1() {
    echo "T1: the corpus"
    while read -r path class note; do
        case "$path" in ''|'#'*) continue ;; esac
        name=$(echo "$path" | sed 's|/|_|g; s|\.mac$||')
        case $class in
        words)       check "$WORK/t1" "$name" "$ROOT/$path" ;;
        words-lf)    check "$WORK/t1" "$name" "$ROOT/$path" lf ;;
        words-title) check "$WORK/t1" "$name" "$ROOT/$path" "" title ;;
        refuse)
            if refusal "$WORK/t1" "$name" "$ROOT/$path"; then
                pass "$path refused at line $RLINE ($note)"
            else
                fail "$path should be refused ($note)"
            fi
            continue ;;
        am1)
            timeout 30 "$MTOA" -o "$WORK/t1/$name.am1" "$ROOT/$path" > "$WORK/t1/$name.mtoa.log" 2>&1
            rc=$?
            ( cd "$WORK/t1" && rm -f "$name.dmp" && timeout 60 "$AM1" -T "$name.am1" > "$name.am1.log" 2>&1 )
            if [ $rc -eq 0 ] && [ -f "$WORK/t1/$name.dmp" ]; then
                pass "$path assembles in am1 ($note)"
            else
                fail "$path: converter rc $rc, am1 $(head -1 "$WORK/t1/$name.am1.log") ($note)"
            fi
            continue ;;
        *) fail "$path: unknown class $class"; continue ;;
        esac
        rc=$?
        if [ $rc -eq 0 ] && [ "$(cat "$WORK/t1/$name.mtoa.rc")" = 0 ]; then
            pass "$path: $(tail -1 "$WORK/t1/$name.why")"
        else
            fail "$path: $(head -2 "$WORK/t1/$name.why" | tr '\n' ' ') [mactoam1 rc $(cat "$WORK/t1/$name.mtoa.rc" 2>/dev/null)]"
        fi
    done < "$HERE/corpus.txt"
}

t2() {
    echo "T2: construct fixtures"
    for f in "$HERE"/fixtures/*.mac; do
        [ -e "$f" ] || continue
        name=$(basename "$f" .mac)
        lf=""
        # a file whose lines end in a bare CR has no LF at all
        if [ "$(tr -cd '\r' < "$f" | wc -c)" -gt 0 ] && [ "$(tr -cd '\n' < "$f" | wc -c)" -eq 0 ]; then
            lf=lf
        fi
        check "$WORK/t2" "$name" "$f" $lf
        rc=$?
        if [ $rc -eq 0 ] && [ "$(cat "$WORK/t2/$name.mtoa.rc")" = 0 ]; then
            pass "$name"
        else
            fail "$name: $(head -2 "$WORK/t2/$name.why" | tr '\n' ' ')"
        fi
    done
}

t3() {
    echo "T3: refusals"
    for f in "$HERE"/refuse/*.mac; do
        [ -e "$f" ] || continue
        name=$(basename "$f" .mac)
        want=$(grep -n '/ *refused' "$f" | head -1 | cut -d: -f1)
        if refusal "$WORK/t3" "$name" "$f" && [ "$RLINE" = "$want" ]; then
            pass "$name refused at line $RLINE"
        else
            fail "$name: expected a refusal at line $want, got line '${RLINE}' ($(grep -m1 error "$WORK/t3/$name.mtoa.log"))"
        fi
        # Control: without the marked line it must convert and agree with macro1.
        # The stripped copy is not named NAME-ok.*: check clears those first.
        grep -v '/ *refused' "$f" > "$WORK/t3/stripped-$name.mac"
        check "$WORK/t3" "$name-ok" "$WORK/t3/stripped-$name.mac"
        rc=$?
        if [ $rc -eq 0 ] && [ "$(cat "$WORK/t3/$name-ok.mtoa.rc")" = 0 ]; then
            pass "$name without the refused line"
        else
            fail "$name without the refused line: $(head -1 "$WORK/t3/$name-ok.why")"
        fi
    done
}

control() {
    echo "Control legs"
    local d=$WORK/control old=$WORK/control/old
    rm -rf "$old"; mkdir -p "$old"
    # The old converter is the last commit that has mactoam1.l: the newest commit touching it,
    # or that commit's parent once the file has been deleted.
    local rev
    rev=$(git -C "$ROOT" rev-list -1 HEAD -- Tools/MtoA/mactoam1.l)
    [ -n "$rev" ] || { fail "no commit has the old converter"; return; }
    git -C "$ROOT" cat-file -e "$rev:Tools/MtoA/mactoam1.l" 2>/dev/null || rev=$rev^
    for f in mactoam1.l symtab.c symtab.h; do
        git -C "$ROOT" show "$rev:Tools/MtoA/$f" > "$old/$f" 2>/dev/null || { fail "old $f not in git $rev"; return; }
    done
    ( cd "$old" && flex --outfile mactoam1.c mactoam1.l && gcc -w -o mactoam1 mactoam1.c symtab.c ) > "$old/make.log" 2>&1 \
        || { fail "old converter does not build"; return; }
    local saved=$MTOA
    MTOA=$old/mactoam1
    # Both assemble cleanly in am1 after the old conversion, with words that differ (a space
    # that adds in macro1 ORs in am1).
    for path in src/blincolnlights/pdp1/maindec/maindec1_14.mac src/blincolnlights/pdp1/maindec/maindec1_20.mac; do
        name=old_$(basename "$path" .mac)
        if check "$d" "$name" "$ROOT/$path"; then
            fail "old converter passes $path"
        else
            pass "old converter fails $path: $(head -1 "$d/$name.why")"
        fi
    done
    for f in "$HERE"/fixtures/*.mac; do
        [ -e "$f" ] || continue
        head -1 "$f" | grep -q 'old fails' || continue
        name=old_$(basename "$f" .mac)
        if check "$d" "$name" "$f"; then
            fail "old converter passes fixture $(basename "$f")"
        else
            pass "old converter fails fixture $(basename "$f")"
        fi
    done
    # One word changed by hand in a passing conversion must fail the compare. The old
    # converter's maindec1_12 passes, so this leg does not depend on the converter under test.
    name=hand_maindec1_12
    if check "$d" "$name" "$ROOT/src/blincolnlights/pdp1/maindec/maindec1_12.mac"; then
        python3 -B -c '
import sys
lines = open(sys.argv[1]).read().split("\n")
a, v = lines[2].split()
lines[2] = "%s %06o" % (a, int(v, 8) ^ 1)
open(sys.argv[2], "w").write("\n".join(lines))' "$d/$name.dmp" "$d/$name.bad.dmp"
        if python3 -B "$HERE/m1cmp.py" "$d/$name.rim" "$d/$name.lst" "$d/$name.bad.dmp" > "$d/$name.bad.why"; then
            fail "a hand-changed word passes the compare"
        else
            pass "a hand-changed word fails: $(head -1 "$d/$name.bad.why")"
        fi
    else
        fail "maindec1_12 does not pass, so the hand-changed control cannot run"
    fi
    MTOA=$saved
    # The same for a word in a constants pool, which the compare matches by value: one pool
    # word changed by hand must fail. Any passing conversion with a pool serves.
    name=hand_constants
    if check "$d" "$name" "$HERE/fixtures/constants.mac"; then
        python3 -B -c '
import sys
sys.path.insert(0, sys.argv[1])
import m1cmp
s = min(m1cmp.pool_starts(sys.argv[2]))
lines = open(sys.argv[3]).read().split("\n")
for i, l in enumerate(lines[:-1]):
    f = l.split()
    if len(f) == 2 and int(f[0], 8) == s:
        lines[i] = "%s %06o" % (f[0], int(f[1], 8) ^ 1)
open(sys.argv[4], "w").write("\n".join(lines))' "$HERE" "$d/$name.lst" "$d/$name.dmp" "$d/$name.bad.dmp"
        if python3 -B "$HERE/m1cmp.py" "$d/$name.rim" "$d/$name.lst" "$d/$name.bad.dmp" > "$d/$name.bad.why"; then
            fail "a hand-changed pool word passes the compare"
        else
            pass "a hand-changed pool word fails: $(head -1 "$d/$name.bad.why")"
        fi
    else
        fail "the constants fixture does not pass, so the hand-changed pool control cannot run"
    fi
}

for t in $TARGETS; do
    case $t in
    t1|t2|t3|control) $t ;;
    *) echo "unknown target $t"; exit 2 ;;
    esac
done
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]

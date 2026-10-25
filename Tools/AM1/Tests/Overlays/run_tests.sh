#!/bin/bash
# run_tests.sh -- the drum overlay suite: fixtures F1 to F13, one check per
# MANIFEST line.
#
# Usage: run_tests.sh [am1] [-i includes] [name...]
#   am1       the am1 to test, default ../../am1 relative to this script (the
#             tree's own build)
#   -i DIR    the include root, default ../../../../Am1Includes relative to
#             this script (the checkout's), so that the F7 fixtures take
#             DRUM/overlay.ah and DRUM/overlay.ac from the tree under test
#             and never from an installed copy
#   name...   run only the MANIFEST lines of these fixtures
#
# MANIFEST holds one check per line, name|expect|flags|text, written by
# make_fixtures.py.  Each check assembles <name>.am1 with -T and the flags, in
# a directory of its own.  When the first flag is "bin" it is dropped and -T
# is not given, so that the binary generator runs: the checks only it makes,
# a root word in a window among them, are never made under -T.  expect is:
#   ok       exits 0
#   okgrep   exits 0, and the .dmp has a line matching the extended regex text
#   lstgrep  exits 0, and the .lst has a line matching the extended regex text;
#            its flags must make a listing ("bin -l")
#   outgrep  exits 0, and stdout has a line matching the extended regex text
#   optgrep  exits 0, and the optimizer's report, the .opt, has a line matching
#            the extended regex text; its flags must include -O, -O1 or -O2
#   nooptgrep  exits 0, a .opt is written, and no line of it matches text
#   ovlgrep  exits 0, and the .ovl has a line matching the extended regex text;
#            its flags must make a tape ("bin")
#   same     exits 0, and the file <name>.<text> written is <name>.<text>.ref,
#            once this directory is taken out of the file names it holds
#   refsub   exits 0, and every line of <name>.ref is a line of the .dmp; the
#            .ref holds the words worked out by hand, which the rest of the
#            dump may add to
#   fail     exits 1, and stderr holds text, so that a fixture that fails
#            for some other reason fails its check; any other non-zero exit
#            is a crash (134 is SIGABRT), not the error the line names
#   warn     exits 0, and stderr holds text
#   nowarn   exits 0, and stderr does not hold text
#   pairs    exits 0, and the .dmp's words agree in pairs; text is "A B N", N
#            words from A, each against the word at the same place from B, all
#            octal; A may be "A/name", the words of overlay name at A
#   nopairs  as pairs, but some pair must disagree, every word being there:
#            the control that shows a pairs check can fail
#   hand     exits 0, and the file <name>.<text> written is the one written by
#            hand/<name>.am1, the optimizer's rewrites made by hand, assembled
#            the same way without its -O flags, once the directories are taken
#            out of the file names each holds
#   nohand   as hand, but the two files must differ: the control that shows a
#            hand check can fail
#   deref    exits 0, and the root word at A is an instruction OP, its top six
#            bits, whose address names, in A's bank, a word holding V; text is
#            "A OP V", all octal.  An %%ovresident's literals are placed by
#            their pool keys, so a check reads through the word, not at a place
#   srccheck exits 0, and so does -O=source with the flags on a copy of the
#            fixture, and the .opt.am1 it writes assembles with -T alone to the
#            words of that build's own .dmp
#
# Exit status 0 when every check passes, 1 when one fails, 2 on a usage error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AM1=""
INC=""
NAMES=()

while [ $# -gt 0 ]; do
    case "$1" in
        -i) INC="$2"; shift 2 ;;
        -*) echo "usage: run_tests.sh [am1] [-i includes] [name...]"; exit 2 ;;
        # The first word that names no fixture is the am1.
        *)  if [ -z "$AM1" ] && [ "${#NAMES[@]}" -eq 0 ] && [ ! -f "$SCRIPT_DIR/$1.am1" ]; then
                AM1="$1"
            else
                NAMES+=("$1")
            fi
            shift ;;
    esac
done
AM1="${AM1:-"$SCRIPT_DIR/../../am1"}"
INC="${INC:-"$SCRIPT_DIR/../../../../Am1Includes"}"

if [ ! -x "$AM1" ] || [ ! -f "$INC/DRUM/overlay.ah" ] || [ ! -f "$INC/DRUM/overlay.ac" ]; then
    echo "usage: run_tests.sh [am1] [-i includes] [name...]"
    echo "am1: $AM1 (must be executable)"
    echo "includes: $INC (must hold DRUM/overlay.ah and DRUM/overlay.ac)"
    exit 2
fi
# Absolute, because each check runs in a directory of its own.
AM1="$(cd "$(dirname "$AM1")" && pwd)/$(basename "$AM1")"
INC="$(cd "$INC" && pwd)"
FX="$SCRIPT_DIR"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
W="$WORK/check"

echo "am1: $AM1"
echo "includes: $INC"
echo

# The pairs of a pairs check that disagree, or are not in the dump, each as
# "missing ..." or "A value, B value", ";"-separated; nothing when all agree.
# $1 the .dmp, $2 "A" or "A/name", $3 B, $4 N.  A root word's dump line is
# "aaaaaa vvvvvv", an overlay's "aaaaaa vvvvvv name".
pair_diffs() {
    local a="${2%%/*}"
    local tag=""
    [ "$2" != "$a" ] && tag="${2#*/}"
    awk -v a="$a" -v tag="$tag" -v b="$3" -v n="$4" '
        function oct(s,    i, v) { v = 0; for (i = 1; i <= length(s); i++) v = v * 8 + substr(s, i, 1); return v }
        NF == 2 { root[oct($1)] = $2 }
        NF == 3 { ov[oct($1) " " $3] = $2 }
        END {
            for (i = 0; i < oct(n); i++) {
                ka = oct(a) + i
                kb = oct(b) + i
                va = (tag == "") ? root[ka] : ov[ka " " tag]
                vb = root[kb]
                if (va == "" || vb == "") {
                    printf "missing %06o or %06o; ", ka, kb
                } else if (va != vb) {
                    printf "%06o %s, %06o %s; ", ka, va, kb, vb
                }
            }
        }' "$1"
}

# What is wrong with a deref check, nothing when it holds.
# $1 the .dmp, $2 A, $3 OP, $4 V.
deref_diff() {
    awk -v a="$2" -v op="$3" -v v="$4" '
        function oct(s,    i, n) { n = 0; for (i = 1; i <= length(s); i++) n = n * 8 + substr(s, i, 1); return n }
        NF == 2 { root[oct($1)] = $2 }
        END {
            w = root[oct(a)]
            if (w == "") { printf "no word at %s", a; exit }
            got = int(oct(w) / 4096) % 64
            if (got != int(oct(op) / 4096) % 64) { printf "%s holds %s, not a %s instruction", a, w, op; exit }
            t = int(oct(a) / 4096) * 4096 + oct(w) % 4096
            if (root[t] == "") { printf "%s names %06o, which holds no word", a, t; exit }
            if (oct(root[t]) != oct(v)) { printf "%s names %06o, which holds %s, not %s", a, t, root[t], v }
        }' "$1"
}

pass=0
fail=0
FAILURES=()

while IFS='|' read -r name expect flags text; do
    case "$name" in
        ''|'#'*) continue ;;
    esac

    if [ "${#NAMES[@]}" -gt 0 ]; then
        hit=0
        for n in "${NAMES[@]}"; do
            [ "$n" = "$name" ] && hit=1
        done
        [ $hit -eq 0 ] && continue
    fi

    T=-T
    if [ "${flags%% *}" = "bin" ]; then
        T=""
        flags="${flags#bin}"
    fi

    rm -rf "$W"
    mkdir -p "$W"
    # $T and $flags are split on purpose, into their options.
    (cd "$W" && "$AM1" -i "$INC" $T $flags "$FX/$name.am1" > out.txt 2> err.txt; echo $? > rc.txt)
    rc=$(cat "$W/rc.txt")
    ok=1
    why=""

    case "$expect" in
        ok)
            [ "$rc" = 0 ] || { ok=0; why="exit $rc"; }
            ;;
        okgrep)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif ! grep -E -q "$text" "$W/$name.dmp" 2>/dev/null; then
                ok=0; why="no dump line matches $text"
            fi
            ;;
        lstgrep)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif ! grep -E -q -- "$text" "$W/$name.lst" 2>/dev/null; then
                ok=0; why="no listing line matches $text"
            fi
            ;;
        outgrep)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif ! grep -E -q -- "$text" "$W/out.txt" 2>/dev/null; then
                ok=0; why="no stdout line matches $text"
            fi
            ;;
        optgrep|ovlgrep)
            ext=opt
            [ "$expect" = ovlgrep ] && ext=ovl
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif ! grep -E -q -- "$text" "$W/$name.$ext" 2>/dev/null; then
                ok=0; why="no .$ext line matches $text"
            fi
            ;;
        nooptgrep)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif [ ! -s "$W/$name.opt" ]; then
                ok=0; why="no .opt"
            elif grep -E -q -- "$text" "$W/$name.opt"; then
                ok=0; why="a .opt line matches $text: $(grep -E -m1 -- "$text" "$W/$name.opt")"
            fi
            ;;
        same)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif [ ! -s "$FX/$name.$text.ref" ] || [ ! -f "$W/$name.$text" ]; then
                ok=0; why="no .$text.ref or no .$text"
            elif ! sed "s#$FX/##" "$W/$name.$text" | cmp -s - "$FX/$name.$text.ref"; then
                ok=0; why="the .$text differs: $(sed "s#$FX/##" "$W/$name.$text" | diff - "$FX/$name.$text.ref" | head -4 | tr '\n' ' ')"
            fi
            ;;
        refsub)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif [ ! -s "$FX/$name.ref" ] || [ ! -f "$W/$name.dmp" ]; then
                ok=0; why="no .ref or no .dmp"
            else
                missing=$(grep -v -x -F -f "$W/$name.dmp" "$FX/$name.ref")
                if [ -n "$missing" ]; then
                    ok=0; why="not in the dump: $(echo $missing)"
                fi
            fi
            ;;
        pairs|nopairs)
            # $text is split on purpose, into A, B and N.
            diffs=""
            [ "$rc" = 0 ] && [ -f "$W/$name.dmp" ] && diffs=$(pair_diffs "$W/$name.dmp" $text)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif [ ! -s "$W/$name.dmp" ]; then
                ok=0; why="no .dmp"
            elif [ "$expect" = pairs ] && [ -n "$diffs" ]; then
                ok=0; why="$diffs"
            elif [ "$expect" = nopairs ] && { [ -z "$diffs" ] || [ "${diffs#*missing}" != "$diffs" ]; }; then
                ok=0; why="every pair agrees, or a word is missing: $diffs"
            fi
            ;;
        hand|nohand)
            # The hand build: the same flags, -O and its forms taken out.
            hflags=""
            for f in $flags; do
                case "$f" in
                    -O*) ;;
                    *)   hflags="$hflags $f" ;;
                esac
            done
            mkdir -p "$W/hand"
            # $T and $hflags are split on purpose, into their options.
            (cd "$W/hand" && "$AM1" -i "$INC" $T $hflags "$FX/hand/$name.am1" > out.txt 2> err.txt; echo $? > rc.txt)
            hrc=$(cat "$W/hand/rc.txt" 2>/dev/null)
            if [ "$rc" != 0 ] || [ "$hrc" != 0 ]; then
                ok=0; why="exit $rc, the hand build's $hrc"
            elif [ ! -s "$W/$name.$text" ] || [ ! -s "$W/hand/$name.$text" ]; then
                ok=0; why="no .$text from one of the builds"
            elif cmp -s <(sed "s#$FX/##" "$W/$name.$text") <(sed "s#$FX/hand/##" "$W/hand/$name.$text"); then
                [ "$expect" = nohand ] && { ok=0; why="the .$text is the hand build's"; }
            else
                [ "$expect" = hand ] && { ok=0; why="the .$text differs from the hand build's: $(cmp <(sed "s#$FX/##" "$W/$name.$text") <(sed "s#$FX/hand/##" "$W/hand/$name.$text") | head -1)"; }
            fi
            ;;
        deref)
            # $text is split on purpose, into A, OP and V.
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif [ ! -s "$W/$name.dmp" ]; then
                ok=0; why="no .dmp"
            else
                diffs=$(deref_diff "$W/$name.dmp" $text)
                [ -n "$diffs" ] && { ok=0; why="$diffs"; }
            fi
            ;;
        srccheck)
            # -O=source writes beside its source, so a copy is assembled, never the fixture.
            mkdir -p "$W/src/re"
            cp "$FX/$name.am1" "$W/src/"
            # $flags is split on purpose, into its options.
            (cd "$W/src" && "$AM1" -i "$INC" -T $flags -O=source "$W/src/$name.am1" > out.txt 2> err.txt; echo $? > rc.txt)
            src=$(cat "$W/src/rc.txt" 2>/dev/null)
            [ -f "$W/src/$name.opt.am1" ] &&
                (cd "$W/src/re" && "$AM1" -i "$INC" -T "$W/src/$name.opt.am1" > out.txt 2> err.txt; echo $? > rc.txt)
            rec=$(cat "$W/src/re/rc.txt" 2>/dev/null)
            if [ "$rc" != 0 ] || [ "$src" != 0 ]; then
                ok=0; why="exit $rc, under -O=source $src"
            elif [ "$rec" != 0 ]; then
                ok=0; why="the .opt.am1 does not assemble: $(head -2 "$W/src/re/err.txt" | tr '\n' ' ')"
            elif [ ! -s "$W/src/$name.dmp" ] || ! cmp -s "$W/src/$name.dmp" "$W/src/re/$name.opt.dmp"; then
                ok=0; why="the .opt.am1's words differ: $(diff "$W/src/$name.dmp" "$W/src/re/$name.opt.dmp" 2>&1 | head -4 | tr '\n' ' ')"
            fi
            ;;
        fail)
            if [ "$rc" = 0 ]; then
                ok=0; why="assembled"
            elif [ "$rc" != 1 ]; then
                ok=0; why="exit $rc"
            elif ! grep -F -q -- "$text" "$W/err.txt"; then
                ok=0; why="failed for another reason"
            fi
            ;;
        warn)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif ! grep -F -q -- "$text" "$W/err.txt"; then
                ok=0; why="no warning"
            fi
            ;;
        nowarn)
            if [ "$rc" != 0 ]; then
                ok=0; why="exit $rc"
            elif grep -F -q -- "$text" "$W/err.txt"; then
                ok=0; why="warned"
            fi
            ;;
        *)
            ok=0; why="unknown expectation $expect"
            ;;
    esac

    if [ $ok = 1 ]; then
        pass=$((pass+1))
        echo "PASS:  $name $expect $text"
    else
        fail=$((fail+1))
        FAILURES+=("$name $expect")
        echo "FAIL:  $name $expect $text: $why"
        sed 's/^/       /' "$W/err.txt" | head -6
    fi
done < "$FX/MANIFEST"

# A name that matches no line runs nothing, and nothing is not a pass.
if [ $((pass + fail)) -eq 0 ]; then
    fail=1
    FAILURES+=("no check ran: ${NAMES[*]}")
fi

echo
echo "Results: $pass pass, $fail fail"
for f in "${FAILURES[@]}"; do
    echo "  failed: $f"
done

[ $fail -eq 0 ] || exit 1
exit 0

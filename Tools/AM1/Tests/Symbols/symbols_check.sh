#!/bin/bash
# symbols_check.sh -- check the line a global label, or a var, is recorded on
# in the symbol file.
#
# THE ONE THING THIS SUITE EXISTS TO HOLD:
#
#     A GLOBAL LABEL'S LINE IN THE .sym IS THE LINE IT IS DEFINED ON, IN THE
#     FILE IT IS WRITTEN IN, WHATEVER FOLLOWS THE LABEL.
#
# "Whatever follows" is the hard part.  lineno counts the line TERMINATOR, so
# what it holds when the label rule reduces depends on whether bison has taken
# the terminator as its lookahead, which depends on what came after the label.
# The line is recorded where the label's comma is SCANNED (lexer.l, parser.y),
# which is on the label's own line in every shape.
#
# A var name is a global symbol too, and the same holds for it with "declared"
# for "defined": its line is recorded where the var's name is scanned, not
# where setVarsPC() places the var.
#
# What can go wrong: a shape nobody thought of; a label in an included file
# taking a line in the includer; an imported symbol losing the line its own
# source gave it; and the line recording changing something other than this
# field.
#
# The checks:
#
#   1. THE SHAPES.  Every (name, line) pair in sym_shapes_test.sym is the one
#      sym_shapes_test.expected states -- fifteen labels, one per shape, and
#      no extra and no missing name.  Likewise sym_var_test.sym against
#      sym_var_test.expected -- ten vars, one or two per shape, one of them in
#      an included file, and two labels.
#   2. The expectations are the source's own numbering: awk finds each label
#      and each var in the source and agrees with the .expected file, for all
#      three.  A mistake in either one is caught by the other.
#   3. ACROSS FILES.  sym_inc_test.am1 includes sym_inc_part.ah; every label
#      takes its line in the file it is written in, and the includer picks its
#      own numbering up again afterwards.
#   4. THE IMPORTER.  sym_lib_test.am1 exports a label on a line the importer
#      does not have; sym_use_test.am1 imports it, and the imported symbol
#      carries that line.
#   5. Nothing else in the .sym moved: the addresses and flags of all three
#      builds are what a plain build gives, which is what makes check 1 a
#      statement about the line field alone.
#   6. (-p only) THE CONTROL LEGS: with the am1 named by -p, one that records
#      these lines wrongly, check 1 FAILS for both sources, and it fails on
#      the shapes named for each -- so check 1 can fail.
#   7. (-p only) ONLY THE LINE FIELD DIFFERS: for each source, every output
#      of the two binaries is byte-identical except the .sym, and the .sym
#      differs only in its last field.
#   8. The coverage guard: the run saw every shape, the vars, both files, the
#      importer and, when -p was given, a control leg that failed for each.
#
# Usage: symbols_check.sh [am1] [-p pre-change-am1]
#   am1   the am1 to test, default ../../am1test relative to this script (the
#         testing build: make am1test)
#   -p    also run checks 6 and 7 against this am1, an older one that gets the
#         labels and the vars wrong (one that gets only the vars wrong gets
#         the labels right, so check 6's shapes leg fails against it, as it
#         should)
#
# Exit status 0 when everything passes, 1 otherwise.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AM1=""
PRE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -p) PRE="$2"; shift 2 ;;
        -*) echo "usage: symbols_check.sh [am1] [-p pre-change-am1]"; exit 2 ;;
        *)  AM1="$1"; shift ;;
    esac
done
AM1="${AM1:-"$SCRIPT_DIR/../../am1test"}"

for b in "$AM1" $PRE; do
    if [ ! -x "$b" ]; then
        echo "am1 binary not found or not executable: $b"
        exit 1
    fi
done
AM1="$(cd "$(dirname "$AM1")" && pwd)/$(basename "$AM1")"
[ -n "$PRE" ] && PRE="$(cd "$(dirname "$PRE")" && pwd)/$(basename "$PRE")"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
FAILURES=()
SEEN=" "
note_pass() { pass=$((pass+1)); }
note_fail() { fail=$((fail+1)); FAILURES+=("$1"); }
seen() { SEEN="$SEEN$1 "; }

echo "am1: $AM1"
[ -n "$PRE" ] && echo "pre: $PRE"
echo

# Assemble SRC with the binary BIN in a directory of its own.  The sources are
# copied in, because am1 writes its outputs beside its cwd and the include of
# sym_inc_part.ah is resolved from there.
# Arguments: BIN DIR SRC [SRC...]
build()
{
    local bin="$1" dir="$2"
    shift 2
    rm -rf "$dir"
    mkdir -p "$dir"
    cp "$SCRIPT_DIR"/*.am1 "$SCRIPT_DIR"/*.ah "$dir/"
    ( cd "$dir" && "$bin" -s "$1" > out 2> err; echo $? > rc )
}

# The (name, line) pairs of a .sym, sorted: "name line".
symLines()
{
    awk 'NR > 3 && NF >= 4 { print $3, $4 }' "$1" | sort
}

# The (name, address, flag) triples of a .sym, sorted: "name addr flag".
symAddrs()
{
    awk 'NR > 3 && NF >= 4 { print $3, $1, $2 }' "$1" | sort
}

# The (name, line) pairs an .expected file states, sorted.
expectedLines()
{
    sed 's/#.*//' "$1" | awk 'NF >= 2 { print $1, $2 }' | sort
}

# The (name, line) pairs awk finds in a source: a label is a name followed by
# a comma at the very start of a line.  Only global labels, which is every
# label in these sources.
labelsIn()
{
    awk '/^[A-Za-z_][A-Za-z0-9_]*,/ {
            n = $0; sub(/,.*/, "", n); print n, FNR }' "$@" | sort
}

# The same for labels and vars: a var is each name in a "var" statement's
# list, less any "=value", on the line the statement is on.
namesIn()
{
    {
        labelsIn "$@"
        awk '/^[ \t]*var[ \t]/ {
                s = $0; sub(/\/\/.*/, "", s); sub(/^[ \t]*var[ \t]+/, "", s)
                n = split(s, v, ",")
                for( i = 1; i <= n; i++ ) {
                    sub(/=.*/, "", v[i]); gsub(/[ \t]/, "", v[i])
                    if( v[i] != "" ) print v[i], FNR
                }
             }' "$@"
    } | sort
}

# ---------------------------------------------------------------------------
# Check 1: the shapes.
# ---------------------------------------------------------------------------

echo "== 1. the shapes"
for leg in "shapes sym_shapes_test labels" "vars sym_var_test labels and vars"; do
    set -- $leg
    d="$1" base="$2"
    shift 2
    what="$*"
    build "$AM1" "$WORK/$d" "$base.am1"
    if [ "$(cat "$WORK/$d/rc")" != 0 ]; then
        echo "FAIL:  $base.am1 did not assemble:"
        sed 's/^/       /' "$WORK/$d/err" | head -5
        note_fail "$d: did not assemble"
    else
        got="$(symLines "$WORK/$d/$base.sym")"
        want="$(expectedLines "$SCRIPT_DIR/$base.expected")"
        if [ "$got" = "$want" ]; then
            echo "PASS:  all $(echo "$want" | wc -l) $what are on the line the source puts them on"
            note_pass
            seen "$d"
        else
            echo "FAIL:  the .sym disagrees with $base.expected:"
            diff <(echo "$want") <(echo "$got") | sed 's/^/       /' | head -30
            note_fail "$d"
        fi
    fi
done

# ---------------------------------------------------------------------------
# Check 2: the expectations are the sources' own numbering.
# ---------------------------------------------------------------------------

echo
echo "== 2. the expectations are the sources own numbering"
for pair in "sym_shapes_test.expected sym_shapes_test.am1" \
            "sym_inc_test.expected sym_inc_test.am1 sym_inc_part.ah" \
            "sym_var_test.expected sym_var_test.am1 sym_var_part.ah"; do
    set -- $pair
    exp="$1"
    shift
    srcs=""
    for s in "$@"; do srcs="$srcs $SCRIPT_DIR/$s"; done
    if [ "$(expectedLines "$SCRIPT_DIR/$exp")" = "$(namesIn $srcs)" ]; then
        echo "PASS:  $exp states what awk finds in $*"
        note_pass
    else
        echo "FAIL:  $exp does not match what awk finds in $*:"
        diff <(expectedLines "$SCRIPT_DIR/$exp") <(namesIn $srcs) | sed 's/^/       /' | head -20
        note_fail "expected/$exp"
    fi
done

# ---------------------------------------------------------------------------
# Check 3: across files.
# ---------------------------------------------------------------------------

echo
echo "== 3. across files"
build "$AM1" "$WORK/inc" sym_inc_test.am1
if [ "$(cat "$WORK/inc/rc")" != 0 ]; then
    echo "FAIL:  sym_inc_test.am1 did not assemble:"
    sed 's/^/       /' "$WORK/inc/err" | head -5
    note_fail "inc: did not assemble"
else
    got="$(symLines "$WORK/inc/sym_inc_test.sym")"
    want="$(expectedLines "$SCRIPT_DIR/sym_inc_test.expected")"
    if [ "$got" = "$want" ]; then
        echo "PASS:  each label takes its line in the file it is written in"
        note_pass
        seen files
    else
        echo "FAIL:  the .sym disagrees with sym_inc_test.expected:"
        diff <(echo "$want") <(echo "$got") | sed 's/^/       /' | head -20
        note_fail "inc"
    fi
fi

# ---------------------------------------------------------------------------
# Check 4: the importer.
# ---------------------------------------------------------------------------

echo
echo "== 4. the importer"
build "$AM1" "$WORK/use" sym_lib_test.am1
libline="$(labelsIn "$SCRIPT_DIR/sym_lib_test.am1" | awk '$1 == "libsym" { print $2 }')"
bad=""
[ "$(cat "$WORK/use/rc")" = 0 ] || bad="$bad the library did not assemble;"
if [ -z "$bad" ]; then
    grep -q "^[0-7]* X libsym $libline\$" "$WORK/use/sym_lib_test.sym" ||
        bad="$bad the library .sym does not put libsym on line $libline;"
    ( cd "$WORK/use" && "$AM1" -s sym_use_test.am1 > out2 2> err2; echo $? > rc2 )
    [ "$(cat "$WORK/use/rc2")" = 0 ] || bad="$bad the importer did not assemble;"
fi
if [ -z "$bad" ]; then
    grep -q "^[0-7]* I libsym $libline\$" "$WORK/use/sym_use_test.sym" ||
        bad="$bad the imported libsym is not on line $libline;"
    useline="$(labelsIn "$SCRIPT_DIR/sym_use_test.am1" | awk '$1 == "usefirst" { print $2 }')"
    grep -q "^[0-7]* G usefirst $useline\$" "$WORK/use/sym_use_test.sym" ||
        bad="$bad usefirst is not on line $useline;"
fi
if [ -z "$bad" ]; then
    echo "PASS:  an imported symbol carries line $libline, the line its own source defines it on"
    note_pass
    seen import
else
    echo "FAIL:  importer:$bad"
    note_fail "import"
fi

# ---------------------------------------------------------------------------
# Check 5: nothing else in the .sym moved.
# ---------------------------------------------------------------------------

echo
echo "== 5. the addresses and flags"
bad=""
for d in shapes vars inc; do
    for f in "$WORK/$d"/*.sym; do
        [ -f "$f" ] || continue
        # every symbol has a six-digit octal address and one of the three flags
        awk 'NR > 3 { if( $1 !~ /^[0-7][0-7][0-7][0-7][0-7][0-7]$/ || $2 !~ /^[GIX]$/ ) bad = 1 }
             END { exit bad ? 1 : 0 }' "$f" || bad="$bad $(basename "$f");"
    done
done
# the shapes source exports one label, so exactly one X and the rest G
[ "$(awk 'NR > 3 && $2 == "X" { n++ } END { print n + 0 }' "$WORK/shapes/sym_shapes_test.sym")" = 1 ] ||
    bad="$bad the export is not marked X;"
if [ -z "$bad" ]; then
    echo "PASS:  every symbol has its octal address and one of G, I, X, and the export is X"
    note_pass
else
    echo "FAIL:  the .sym is malformed:$bad"
    note_fail "addresses"
fi

# ---------------------------------------------------------------------------
# Check 6: the control legs against the pre-change am1.
# ---------------------------------------------------------------------------

echo
echo "== 6. the control legs (-p)"
if [ -z "$PRE" ]; then
    echo "SKIP:  no -p given, so check 1 has not been shown to be able to fail"
else
    # Each leg: its directory, its source, and the names the pre-change am1
    # must get wrong -- the label shapes whose line depends on what follows
    # the label, and for the vars every shape but afterinc, whose line can
    # coincide with where vars placed it.
    for leg in "shapes sym_shapes_test alone last afterblock fwdtext fwdascii fwd340" \
               "vars sym_var_test alone withcomment pairone pairtwo initd afterblock fwdvar incvar atend"; do
        set -- $leg
        d="$1" base="$2"
        shift 2
        named="$*"
        build "$PRE" "$WORK/pre$d" "$base.am1"
        if [ "$(cat "$WORK/pre$d/rc")" != 0 ]; then
            echo "FAIL:  the pre-change am1 did not assemble $base.am1"
            note_fail "control $d: did not assemble"
            continue
        fi
        pregot="$(symLines "$WORK/pre$d/$base.sym")"
        want="$(expectedLines "$SCRIPT_DIR/$base.expected")"
        if [ "$pregot" = "$want" ]; then
            echo "FAIL:  the pre-change am1 PASSES check 1 on $base, so it cannot fail there"
            note_fail "control $d"
            continue
        fi
        # and it is wrong on the shapes named for it
        wrong="$(join <(echo "$want") <(echo "$pregot") |
                 awk '$2 != $3 { print $1 }' | tr '\n' ' ')"
        bad=""
        for n in $named; do
            case " $wrong " in *" $n "*) ;; *) bad="$bad $n" ;; esac
        done
        if [ -z "$bad" ]; then
            echo "PASS:  the pre-change am1 fails check 1 on $base, on $(echo $wrong | wc -w) names,"
            echo "       among them every shape named for it"
            note_pass
            seen "control$d"
        else
            echo "FAIL:  the pre-change am1 was right on $base about:$bad"
            note_fail "control $d shapes"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Check 7: nothing but the line field differs.
# ---------------------------------------------------------------------------

echo
echo "== 7. nothing but the line field (-p)"
if [ -z "$PRE" ]; then
    echo "SKIP:  no -p given"
else
    bad=""
    for src in sym_shapes_test.am1 sym_inc_test.am1 sym_lib_test.am1 sym_var_test.am1; do
        base="${src%.am1}"
        build "$AM1" "$WORK/new/$base" "$src"
        build "$PRE" "$WORK/old/$base" "$src"
        # two builds that both fail compare equal, and prove nothing
        if [ "$(cat "$WORK/new/$base/rc")" != 0 ] || [ "$(cat "$WORK/old/$base/rc")" != 0 ]; then
            bad="$bad $base did not assemble;"
            continue
        fi
        for f in "$WORK/new/$base"/*; do
            n="$(basename "$f")"
            case "$n" in *.am1|*.ah|rc|out|err) continue ;; esac
            o="$WORK/old/$base/$n"
            [ -f "$o" ] || { bad="$bad $base/$n missing;"; continue; }
            if [ "$n" = "$base.sym" ]; then
                cmp -s <(symAddrs "$f") <(symAddrs "$o") ||
                    bad="$bad $base: an address or flag moved;"
            else
                cmp -s "$f" "$o" || bad="$bad $base/$n differs;"
            fi
        done
    done
    if [ -z "$bad" ]; then
        echo "PASS:  four sources, every output byte-identical, and in the .sym only the line field moved"
        note_pass
        seen onlyline
    else
        echo "FAIL:  something other than the line field moved:$bad"
        note_fail "only the line field"
    fi
fi

# ---------------------------------------------------------------------------
# Check 8: the coverage guard.
# ---------------------------------------------------------------------------

echo
echo "== 8. coverage guard"
missing=""
for s in shapes vars files import; do
    case "$SEEN" in *" $s "*) ;; *) missing="$missing $s" ;; esac
done
if [ -n "$PRE" ]; then
    for s in controlshapes controlvars onlyline; do
        case "$SEEN" in *" $s "*) ;; *) missing="$missing $s" ;; esac
    done
fi
if [ -z "$missing" ]; then
    echo "PASS:  the run saw the shapes, the vars, two files, an importer$([ -n "$PRE" ] && echo ", a failing control leg for each and the byte comparison")"
    note_pass
else
    echo "FAIL:  the run never saw:$missing"
    note_fail "coverage"
fi

echo
echo "Results: $pass pass, $fail fail"
if [ $fail -ne 0 ]; then
    echo "Failed: ${FAILURES[*]}"
    exit 1
fi
exit 0

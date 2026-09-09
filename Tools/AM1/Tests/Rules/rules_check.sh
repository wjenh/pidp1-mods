#!/bin/bash
# rules_check.sh -- acceptance check for the am1 optimizer's rule engine and
# report (task A5).  Task A6 adopts it into the Tests/Optimizer suite.
#
# Six checks:
#   1. "am1 -O=rules" on rules_test.am1 must equal rules_test.expected line for
#      line.  That file is one positive case per rule and the expectations were
#      written by hand from the study's transform catalogue before the engine
#      was run on it, so a difference means one of the two is wrong and
#      somebody has to look.  Every rule must appear in it exactly once (T8d
#      and T14 twice and three times, having two and three forms).
#   2. rules_neg.am1, one negative case per precondition and per rule-specific
#      refusal, must equal rules_neg.expected and must produce NO live finding:
#      every one of its patterns matches and every one of them is refused.
#   3. rules_none.am1 must produce no findings at all and still get a complete
#      report: the precondition notes, both finding sections and the statistics.
#   4. For every assemblable test in the regression suite, the dump's totals
#      line must agree with the report's own counts, and the findings must come
#      out sorted by bank and then by address -- which is what makes the report
#      diffable between runs and usable as a test reference.
#   5. Nothing may be written to stderr by any of it.
#   6. The analysis must be deterministic: two runs on the same source give the
#      same dump.
#
# Usage: rules_check.sh [am1] [regression-dir]
#   am1             the am1 to test, default ../../am1 relative to this script
#   regression-dir  where the rgNN tests live, default Tools/AM1/Tests/Regression
#
# On this machine it runs under WSL bash.  Exit status 0 when everything
# passes, 1 otherwise.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AM1="${1:-"$SCRIPT_DIR/../../am1"}"
REG="${2:-"$SCRIPT_DIR/../../../../Tools/AM1/Tests/Regression"}"

if [ ! -x "$AM1" ]; then
    echo "am1 binary not found or not executable: $AM1"
    exit 1
fi

if [ ! -d "$REG" ]; then
    echo "regression directory not found: $REG"
    exit 1
fi

AM1="$(cd "$(dirname "$AM1")" && pwd)/$(basename "$AM1")"
REG="$(cd "$REG" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
FAILURES=()

cp "$SCRIPT_DIR"/rules_test.am1 "$SCRIPT_DIR"/rules_neg.am1 "$SCRIPT_DIR"/rules_none.am1 "$WORK/"
cd "$WORK" || exit 1

# Every rule of the catalogue, and how many findings rules_test.am1 must
# produce for each.  T8d has two forms (a value and its complement) and T14
# has three (lia, lai and the exchange).
RULES=(T1 T1b T2 T3 T6 T7 T8a T8b T8c T8d T13 T14)
COUNTS=(1 1 1 1 1 1 1 1 1 2 1 3)

# --- 1. the positive cases -----------------------------------------------

"$AM1" -O=rules rules_test.am1 > test.out 2> test.err
rc=$?
if [ $rc -ne 0 ] || [ ! -f rules_test.opt ]; then
    echo "FAIL:  rules_test (assembly with -O=rules failed, rc=$rc)"
    sed 's/^/       /' test.err
    fail=$((fail+1)); FAILURES+=("rules_test")
elif diff -u "$SCRIPT_DIR/rules_test.expected" test.out > test.diff; then
    echo "PASS:  rules_test ($(($(wc -l < test.out) - 1)) findings equal the hand-written expectations)"
    pass=$((pass+1))
else
    echo "FAIL:  rules_test (findings differ from rules_test.expected; - expected, + engine)"
    sed 's/^/       /' test.diff
    fail=$((fail+1)); FAILURES+=("rules_test")
fi

if [ -s test.err ]; then
    echo "FAIL:  rules_test (the analysis wrote to stderr)"
    sed 's/^/       /' test.err
    fail=$((fail+1)); FAILURES+=("rules_test-stderr")
fi

# Every rule fires, the right number of times, and none of them is refused.
for i in "${!RULES[@]}"; do
    rule="${RULES[$i]}"
    want="${COUNTS[$i]}"
    got=$(awk -v r="$rule" '$1 == "live" && $2 == r' test.out | wc -l)
    if [ "$got" -eq "$want" ]; then
        pass=$((pass+1))
    else
        echo "FAIL:  rules_test rule $rule produced $got live findings, expected $want"
        fail=$((fail+1)); FAILURES+=("rules_test-$rule")
    fi
done

if grep -q '^supp ' test.out; then
    echo "FAIL:  rules_test (a positive case was refused)"
    grep '^supp ' test.out | sed 's/^/       /'
    fail=$((fail+1)); FAILURES+=("rules_test-suppressed")
else
    echo "PASS:  rules_test all ${#RULES[@]} rules fire and none is refused"
    pass=$((pass+1))
fi

# --- 2. the negative cases ------------------------------------------------

"$AM1" -O=rules rules_neg.am1 > neg.out 2> neg.err
rc=$?
if [ $rc -ne 0 ] || [ ! -f rules_neg.opt ]; then
    echo "FAIL:  rules_neg (assembly with -O=rules failed, rc=$rc)"
    sed 's/^/       /' neg.err
    fail=$((fail+1)); FAILURES+=("rules_neg")
elif diff -u "$SCRIPT_DIR/rules_neg.expected" neg.out > neg.diff; then
    echo "PASS:  rules_neg ($(($(wc -l < neg.out) - 1)) refusals equal the hand-written expectations)"
    pass=$((pass+1))
else
    echo "FAIL:  rules_neg (refusals differ from rules_neg.expected; - expected, + engine)"
    sed 's/^/       /' neg.diff
    fail=$((fail+1)); FAILURES+=("rules_neg")
fi

if [ -s neg.err ]; then
    echo "FAIL:  rules_neg (the analysis wrote to stderr)"
    sed 's/^/       /' neg.err
    fail=$((fail+1)); FAILURES+=("rules_neg-stderr")
fi

if grep -q '^live ' neg.out; then
    echo "FAIL:  rules_neg (a case that should have been refused was reported live)"
    grep '^live ' neg.out | sed 's/^/       /'
    fail=$((fail+1)); FAILURES+=("rules_neg-live")
else
    echo "PASS:  rules_neg every pattern in it is refused, none reported live"
    pass=$((pass+1))
fi

# Each of the six shared preconditions and each rule-specific reason must be
# represented, or the file has stopped covering what it claims to.
for why in P1-label P1-entry P2-written P2-taken P3-xct P4-skip \
           phase flags repeat lap exchange count noinverse patched tempused; do
    if awk -v w="$why" '$1 == "supp" && $5 == w' neg.out | grep -q .; then
        pass=$((pass+1))
    else
        echo "FAIL:  rules_neg has no case refused for reason $why"
        fail=$((fail+1)); FAILURES+=("rules_neg-$why")
    fi
done

# --- 3. a source with nothing to say --------------------------------------

"$AM1" -O=rules rules_none.am1 > none.out 2> none.err
rc=$?
if [ $rc -ne 0 ] || [ ! -f rules_none.opt ]; then
    echo "FAIL:  rules_none (assembly with -O=rules failed, rc=$rc)"
    sed 's/^/       /' none.err
    fail=$((fail+1)); FAILURES+=("rules_none")
elif grep -q '^totals live 0 supp 0 ' none.out && [ "$(wc -l < none.out)" -eq 1 ]; then
    echo "PASS:  rules_none produces no findings"
    pass=$((pass+1))
else
    echo "FAIL:  rules_none produced findings"
    sed 's/^/       /' none.out
    fail=$((fail+1)); FAILURES+=("rules_none")
fi

# The report is still complete: the caveat, both sections and the statistics.
missing=""
for want in "P5 CANNOT BE CHECKED HERE" "^Findings: 0" "^Suppressed findings: 0" \
            "^Statistics" "static time" "^  Word table:"; do
    if ! grep -q "$want" rules_none.opt; then
        missing="$missing [$want]"
    fi
done

if [ -z "$missing" ]; then
    echo "PASS:  rules_none still gets a complete report"
    pass=$((pass+1))
else
    echo "FAIL:  rules_none report is missing:$missing"
    fail=$((fail+1)); FAILURES+=("rules_none-report")
fi

# --- 4, 5 and 6: every regression test analyzes ---------------------------

# Runs one source and checks the dump against the report and against itself.
# Arguments: test name, source path, then any extra am1 flags.
run_one()
{
    local name=$1 src=$2
    shift 2
    local flags=("$@")
    local rc live supp rlive rsupp

    "$AM1" "${flags[@]}" -O=rules -I"$REG" "$src" > "$name.ru" 2> "$name.err"
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL:  $name (assembly with -O=rules failed, rc=$rc)"
        sed 's/^/       /' "$name.err"
        fail=$((fail+1)); FAILURES+=("$name")
        return
    fi

    if [ -s "$name.err" ]; then
        echo "FAIL:  $name (the analysis wrote to stderr)"
        sed 's/^/       /' "$name.err"
        fail=$((fail+1)); FAILURES+=("$name")
        return
    fi

    # The dump's own totals against the counts the report prints.
    live=$(sed -n 's/^totals live \([0-9]*\) .*/\1/p' "$name.ru")
    supp=$(sed -n 's/^totals live [0-9]* supp \([0-9]*\) .*/\1/p' "$name.ru")
    rlive=$(sed -n 's/^Findings: \([0-9]*\)$/\1/p' "$name.opt")
    rsupp=$(sed -n 's/^Suppressed findings: \([0-9]*\)$/\1/p' "$name.opt")

    if [ "$live" != "$rlive" ] || [ "$supp" != "$rsupp" ]; then
        echo "FAIL:  $name (dump says $live live / $supp suppressed, report says $rlive / $rsupp)"
        fail=$((fail+1)); FAILURES+=("$name")
        return
    fi

    # The dump's line count must be exactly the findings plus the totals line.
    if [ "$(wc -l < "$name.ru")" -ne $((live + supp + 1)) ]; then
        echo "FAIL:  $name (the dump has $(wc -l < "$name.ru") lines for $live + $supp findings)"
        fail=$((fail+1)); FAILURES+=("$name")
        return
    fi

    # Deliverable 5: findings sorted by bank then address, within each list.
    if ! awk '
        /^totals /  { next }
        {
            bank = $3; sub(/^b/, "", bank)
            key = (bank * 010000) + strtonum("0" $4)
            if( $1 != last )
            {
                last = $1; prev = -1
            }
            if( key < prev ) { print "out of order at " $3 " " $4; bad = 1 }
            prev = key
        }
        END { exit( (bad)?1:0 ) }
    ' "$name.ru" > "$name.order"; then
        echo "FAIL:  $name (findings are not in bank and address order)"
        head -5 "$name.order" | sed 's/^/       /'
        fail=$((fail+1)); FAILURES+=("$name")
        return
    fi

    # Determinism: a second run gives the same dump.
    "$AM1" "${flags[@]}" -O=rules -I"$REG" "$src" > "$name.ru2" 2> /dev/null
    if cmp -s "$name.ru" "$name.ru2"; then
        echo "PASS:  $name ($live live, $supp suppressed)"
        pass=$((pass+1))
    else
        echo "FAIL:  $name (two runs gave different findings)"
        diff "$name.ru" "$name.ru2" | sed 's/^/       /'
        fail=$((fail+1)); FAILURES+=("$name")
    fi
}

# rg21 first: it writes rg21_exports.sym, which rg22 imports.  rg25 needs -M
# (a deliberate overlay) and rg26 -a (space is add).  The xfail tests do not
# assemble and have no words to analyze.
run_one rg21_exports "$REG/rg21_exports.am1"

for src in "$REG"/rg*.am1; do
    name="$(basename "$src" .am1)"
    case "$name" in
        rg21_exports)         continue ;;
        *_xfail_*)            continue ;;
        rg25_flag_M)          run_one "$name" "$src" -M ;;
        rg26_flag_a)          run_one "$name" "$src" -a ;;
        *)                    run_one "$name" "$src" ;;
    esac
done

# The flag-off path: no report, no findings, nothing.
rm -f rules_test.opt
"$AM1" rules_test.am1 > /dev/null 2>&1

if [ -f rules_test.opt ]; then
    echo "FAIL:  a .opt file appeared without -O"
    fail=$((fail+1)); FAILURES+=("flag-off")
else
    echo "PASS:  no .opt is written without -O"
    pass=$((pass+1))
fi

echo
echo "Results: $pass pass, $fail fail"
if [ $fail -ne 0 ]; then
    echo "Failed: ${FAILURES[*]}"
    exit 1
fi
exit 0

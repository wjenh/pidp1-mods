#!/bin/bash
# run_tests.sh - run the multibank test suite.
# Usage: ./run_rtests.sh
# Uses am1 binary in this hierarchy, not the system-installed one.
# Tests are run in a temp directory so no artifacts land in the Multibank folder.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AM1=../../am1

if [ ! -x "$AM1" ]; then
    echo "am1 binary not found or not executable: $AM1"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0; fail=0
declare -a FAILURES

run_test() {
    local name=$1 src=$2 expect_fail=${3:-0}
    "$AM1" -b -I"$SCRIPT_DIR" "$src" > "$WORK/$name.log" 2>&1
    local rc=$?
    if [ "$expect_fail" -eq 1 ]; then
        if [ $rc -ne 0 ]; then echo "XFAIL: $name"; else echo "UNXPASS: $name"; ((fail++)); FAILURES+=("$name"); fi
    elif [ $rc -eq 0 ]; then
        echo "PASS:  $name"
        ((pass++))
    else
        echo "FAIL:  $name"
        sed 's/^/       /' "$WORK/$name.log"
        ((fail++))
        FAILURES+=("$name")
    fi
}

run_test rg1_init_pc              "$SCRIPT_DIR/rg1_init_pc.am1"
run_test rg2_multibank_vars       "$SCRIPT_DIR/rg2_multibank_vars.am1"
run_test rg3_multibank_consts     "$SCRIPT_DIR/rg3_multibank_consts.am1"
run_test rg4_swap_preserves_pc    "$SCRIPT_DIR/rg4_swap_preserves_pc.am1"
run_test rg5_globalSym_multibank  "$SCRIPT_DIR/rg5_globalSym_multibank.am1"

rm *.rim
echo ""
echo "Results: $pass pass, $fail fail"
[ ${#FAILURES[@]} -gt 0 ] && echo "Failures: ${FAILURES[*]}"
[ $fail -eq 0 ]

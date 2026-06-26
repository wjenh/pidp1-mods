#!/bin/bash
# run_regression.sh -- comprehensive am1 regression suite
# Usage: ./run_regression.sh [path-to-am1-binary]
#
# Each passing test is run with -T (test mode) which produces a .dmp file
# containing every stored word as "aaaaaa vvvvvv" (address value in octal).
# The .dmp is diffed against a pre-generated .ref file; a mismatch is a FAIL.
# XFAIL tests are run normally (no -T) and only their exit code is checked.
# All output files go to a temp directory; nothing is left in Regression/.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AM1="${1:-"$SCRIPT_DIR/../../am1"}"

if [ ! -x "$AM1" ]; then
    echo "am1 binary not found or not executable: $AM1"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

pass=0; fail=0; xfail=0
declare -a FAILURES

run_test() {
    local name=$1 src=$2 expect_fail=0
    shift 2
    local flags=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --xfail) expect_fail=1; shift ;;
            *) flags+=("$1"); shift ;;
        esac
    done

    if [ "$expect_fail" -eq 1 ]; then
        "$AM1" "${flags[@]}" -I"$SCRIPT_DIR" "$src" > "$WORK/$name.log" 2>&1
        local rc=$?
        if [ $rc -ne 0 ]; then
            echo "XFAIL: $name"
            xfail=$((xfail+1))
        else
            echo "UNXPASS: $name"
            fail=$((fail+1))
            FAILURES+=("$name")
        fi
        return
    fi

    # Normal test: assemble with -T and compare binary output to reference
    "$AM1" -T "${flags[@]}" -I"$SCRIPT_DIR" "$src" > "$WORK/$name.log" 2>&1
    local rc=$?

    if [ $rc -ne 0 ]; then
        echo "FAIL:  $name (assembly error)"
        sed 's/^/       /' "$WORK/$name.log"
        fail=$((fail+1))
        FAILURES+=("$name")
        return
    fi

    local dmp="$WORK/${name}.dmp"
    local ref="$SCRIPT_DIR/${name}.ref"

    if [ ! -f "$dmp" ]; then
        echo "FAIL:  $name (no .dmp produced)"
        fail=$((fail+1))
        FAILURES+=("$name")
        return
    fi

    if [ ! -f "$ref" ]; then
        echo "FAIL:  $name (no .ref file -- run with --gen-refs to create)"
        fail=$((fail+1))
        FAILURES+=("$name")
        return
    fi

    if diff -q "$dmp" "$ref" > /dev/null 2>&1; then
        echo "PASS:  $name"
        pass=$((pass+1))
    else
        echo "FAIL:  $name (binary mismatch)"
        diff "$ref" "$dmp" | sed 's/^/       /'
        fail=$((fail+1))
        FAILURES+=("$name")
    fi
}

# rg21 must run first -- it generates rg21_exports.sym needed by rg22
run_test rg21_exports         "$SCRIPT_DIR/rg21_exports.am1"

run_test rg01_origin          "$SCRIPT_DIR/rg01_origin.am1"
run_test rg02_comments        "$SCRIPT_DIR/rg02_comments.am1"
run_test rg03_separator       "$SCRIPT_DIR/rg03_separator.am1"
run_test rg04_start           "$SCRIPT_DIR/rg04_start.am1"
run_test rg05_stop            "$SCRIPT_DIR/rg05_stop.am1"
run_test rg06_numbers         "$SCRIPT_DIR/rg06_numbers.am1"
run_test rg07_operators       "$SCRIPT_DIR/rg07_operators.am1"
run_test rg08_ones_complement "$SCRIPT_DIR/rg08_ones_complement.am1"
run_test rg09_symbols         "$SCRIPT_DIR/rg09_symbols.am1"
run_test rg10_locals          "$SCRIPT_DIR/rg10_locals.am1"
run_test rg11_vars            "$SCRIPT_DIR/rg11_vars.am1"
run_test rg12_constants       "$SCRIPT_DIR/rg12_constants.am1"
run_test rg13_table           "$SCRIPT_DIR/rg13_table.am1"
run_test rg14_text            "$SCRIPT_DIR/rg14_text.am1"
run_test rg15_ascii           "$SCRIPT_DIR/rg15_ascii.am1"
run_test rg16_flexo           "$SCRIPT_DIR/rg16_flexo.am1"
run_test rg17_type340         "$SCRIPT_DIR/rg17_type340.am1"
run_test rg18_char            "$SCRIPT_DIR/rg18_char.am1"
run_test rg19_labeled_text    "$SCRIPT_DIR/rg19_labeled_text.am1"
run_test rg20_cpp             "$SCRIPT_DIR/rg20_cpp.am1"
run_test rg22_imports         "$SCRIPT_DIR/rg22_imports.am1"
run_test rg23_wildcard_refs   "$SCRIPT_DIR/rg23_wildcard_refs.am1"
run_test rg24_bank_refs       "$SCRIPT_DIR/rg24_bank_refs.am1"
run_test rg25_flag_M          "$SCRIPT_DIR/rg25_flag_M.am1"   -M
run_test rg26_flag_a          "$SCRIPT_DIR/rg26_flag_a.am1"   -a
run_test rg27_pdp1d           "$SCRIPT_DIR/rg27_pdp1d.am1"
run_test rg28_memory_layout   "$SCRIPT_DIR/rg28_memory_layout.am1"

run_test rg29_xfail_undef_sym  "$SCRIPT_DIR/rg29_xfail_undef_sym.am1"  --xfail
run_test rg30_xfail_overwrite  "$SCRIPT_DIR/rg30_xfail_overwrite.am1"  --xfail
run_test rg31_xfail_bank_undef "$SCRIPT_DIR/rg31_xfail_bank_undef.am1" --xfail

echo ""
echo "Results: $pass pass, $fail fail, $xfail expected-fail"
[ ${#FAILURES[@]} -gt 0 ] && echo "Failures: ${FAILURES[*]}"
[ $fail -eq 0 ]

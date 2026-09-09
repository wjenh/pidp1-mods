#!/bin/bash
# run_regression.sh -- comprehensive am1 regression suite
# Usage: ./run_regression.sh
# All tests run in a temp directory; no artifacts are left in Regression/.
#
# Each test is two checks, not one:
#   1. am1 exits successfully (or, for an --xfail test, does not)
#   2. the words it generated match the stored <name>.ref

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
declare -a NOREF

# Compare what am1 generated against the stored .ref.
#
# This is a SECOND invocation of am1 rather than a -T added to the test's own
# run, and that is deliberate: -T selects a different code generator, and that
# generator does not make every check the binary one does. rg30 is the proof --
# its duplicate 100/ origin is caught and reported without -T, and assembles
# silently with exit 0 under it. Running the test plainly first keeps every
# check the suite already had; the -T run only supplies the word dump.
check_ref() {
    local name=$1 src=$2
    shift 2
    local ref="$SCRIPT_DIR/$name.ref"

    if [ ! -f "$ref" ]; then
        NOREF+=("$name")
        return
    fi

    rm -f "$name.dmp"
    "$AM1" -T "$@" -I"$SCRIPT_DIR" "$src" > "$WORK/$name.T.log" 2>&1

    if [ ! -f "$name.dmp" ]; then
        echo "FAIL:  ${name}_ref (the -T run wrote no .dmp)"
        sed 's/^/       /' "$WORK/$name.T.log"
        fail=$((fail+1))
        FAILURES+=("${name}_ref")
        return
    fi

    if diff -q "$name.dmp" "$ref" > /dev/null 2>&1; then
        echo "PASS:  ${name}_ref"
        pass=$((pass+1))
    else
        echo "FAIL:  ${name}_ref (generated words differ from the stored .ref)"
        echo "       < stored .ref, > generated now"
        diff "$ref" "$name.dmp" | sed 's/^/       /'
        fail=$((fail+1))
        FAILURES+=("${name}_ref")
    fi
}

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

    "$AM1" "${flags[@]}" -I"$SCRIPT_DIR" "$src" > "$WORK/$name.log" 2>&1
    local rc=$?

    if [ "$expect_fail" -eq 1 ]; then
        if [ $rc -ne 0 ]; then
            echo "XFAIL: $name"
            xfail=$((xfail+1))
        else
            echo "UNXPASS: $name"
            fail=$((fail+1))
            FAILURES+=("$name")
        fi
    elif [ $rc -eq 0 ]; then
        echo "PASS:  $name"
        pass=$((pass+1))
        check_ref "$name" "$src" "${flags[@]}"
    else
        echo "FAIL:  $name"
        sed 's/^/       /' "$WORK/$name.log"
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

# rg32 -- constants-pool determinism (28-Aug-26): the old pointer-based
# constant fingerprints made the pool layout track malloc addresses, so the
# same source assembled differently on every run under ASLR. Three checks,
# deliberately WITHOUT any setarch -R wrapper:
#   1. it assembles (run_test as usual)
#   2. two plain runs produce byte-identical .rim output
#   3. the -T dump matches the stored .ref -- this pins the exact pool layout,
#      including the wildcard-ref-dedup-by-name behavior. It is no longer
#      written out here: run_test does it for every test with a .ref. As
#      before, regenerate the .ref only if the fingerprint scheme is
#      deliberately changed.
run_test rg32_determinism      "$SCRIPT_DIR/rg32_determinism.am1"
"$AM1" -I"$SCRIPT_DIR" "$SCRIPT_DIR/rg32_determinism.am1" >/dev/null 2>&1 && cp rg32_determinism.rim rg32_run1.rim
"$AM1" -I"$SCRIPT_DIR" "$SCRIPT_DIR/rg32_determinism.am1" >/dev/null 2>&1
if cmp -s rg32_run1.rim rg32_determinism.rim; then
    echo "PASS:  rg32_two_run_identical"
    pass=$((pass+1))
else
    echo "FAIL:  rg32_two_run_identical (output differs between two plain runs -- ASLR-sensitive again?)"
    fail=$((fail+1))
    FAILURES+=("rg32_two_run_identical")
fi
# rg33 -- placement of the automatically emitted constants and variables blocks
# (05-Sep-26). They were stamped with whatever bank was current when the program
# ended rather than their own, the variables were written past the constants
# because the parser and the code generators disagreed on the order, and each
# generator advanced the bank's cur_pc so the next one used a stale address.
# Two checks, and both are needed:
#   1. the -T dump pins the addresses the parser assigns -- run_test does this
#      now, as it does for every test with a .ref
#   2. the loader block headers in the .rim pin where the binary generator
#      actually writes them. The -T dump comes from a different generator and
#      never showed the wrong-bank defect at all, so check 2 stays here.
# Regenerate the .ref and .blocks files only if the layout is deliberately changed.

# Print the loader block headers of a .rim as "start end" in octal, one per line.
# Words are three bytes with bit 0200 set in each, blocks are separated by zero
# bytes, and the first run of words is the loader itself, so it is skipped.
rim_blocks() {
    od -An -v -tu1 "$1" | awk '
    { for( i = 1; i <= NF; ++i ) {
        b = $i + 0
        if( b >= 128 )
        {
            w = w * 64 + (b % 64)
            if( ++k == 3 ) { word[++nw] = w; w = 0; k = 0 }
        }
        else
        {
            if( nw ) { gap[nw] = 1 }
            w = 0; k = 0
        }
      } }
    END {
        for( i = 1; i <= nw && !gap[i]; ++i ) { }      # skip the loader
        for( ++i; i + 1 <= nw; )
        {
            if( gap[i] ) { ++i; continue }             # the trailing start/stop word
            printf "%06o %06o\n", word[i], word[i+1]
            i += 2 + (word[i+1] - word[i])
        }
    }'
}

run_test rg33_trailing_blocks  "$SCRIPT_DIR/rg33_trailing_blocks.am1"

rim_blocks rg33_trailing_blocks.rim > rg33_trailing_blocks.blk 2>/dev/null
if diff -q rg33_trailing_blocks.blk "$SCRIPT_DIR/rg33_trailing_blocks.blocks" >/dev/null 2>&1; then
    echo "PASS:  rg33_block_placement"
    pass=$((pass+1))
else
    echo "FAIL:  rg33_block_placement (a trailing block landed in the wrong bank or at the wrong address)"
    diff "$SCRIPT_DIR/rg33_trailing_blocks.blocks" rg33_trailing_blocks.blk | sed 's/^/       /'
    fail=$((fail+1))
    FAILURES+=("rg33_block_placement")
fi

# rg34 -- pooled-literal labels in the listing (06-Sep-26). Literals are pooled
# BY VALUE, so two references whose values coincide share one pool word. The
# listing used to render every one of them from the pooled symbol's expression
# tree -- whichever reference interned the slot first -- so a bank-0 call could
# be annotated with an unrelated bank-2 string label. The object code was never
# affected, which is why nothing else in this suite would ever catch it.
# TWO checks, and the second is what keeps the first honest:
#   1. each reference is labelled with what the source wrote
#   2. the two references are still SHARING one pool word. If a change ever
#      stopped them sharing, each would get its own slot, the labels would be
#      right for the wrong reason, and check 1 would go vacuous.
run_test rg34_litpool_labels   "$SCRIPT_DIR/rg34_litpool_labels.am1"  -l

rg34_far=$(grep -c 'lac \[alpha:0\]' rg34_litpool_labels.lst 2>/dev/null)
rg34_near=$(grep -c 'lac \[beta\]' rg34_litpool_labels.lst 2>/dev/null)
if [ "$rg34_far" = "1" ] && [ "$rg34_near" = "1" ]; then
    echo "PASS:  rg34_litpool_label_fidelity"
    pass=$((pass+1))
else
    echo "FAIL:  rg34_litpool_label_fidelity (a pooled literal is labelled with another reference's expression)"
    grep ' lac \[' rg34_litpool_labels.lst 2>/dev/null | sed 's/^/       /'
    fail=$((fail+1))
    FAILURES+=("rg34_litpool_label_fidelity")
fi

# One distinct object word across both references means one shared pool slot.
rg34_words=$(awk '/ lac \[/ { print $3 }' rg34_litpool_labels.lst 2>/dev/null | sort -u | wc -l)
if [ "$rg34_words" = "1" ]; then
    echo "PASS:  rg34_litpool_still_shared"
    pass=$((pass+1))
else
    echo "FAIL:  rg34_litpool_still_shared (the two literals no longer share a pool word -- rg34's label check is now vacuous)"
    grep ' lac \[' rg34_litpool_labels.lst 2>/dev/null | sed 's/^/       /'
    fail=$((fail+1))
    FAILURES+=("rg34_litpool_still_shared")
fi


# rg35 -- the last word of a bank (09-Sep-26). Every directive that reserves a
# BLOCK advanced cur_pc and then bound-checked the result, which is one PAST
# the last word the block used. A block ending exactly on 07777 left cur_pc at
# 010000 and was rejected, so table, text, ascii, type340, variables and
# constants could not reach the top word of any bank -- although a plain
# instruction could, which is the control leg rg35_bank_ceiling carries in
# bank 0. Four checks, because relaxing a bound is only half a fix:
#   1. rg35_bank_ceiling assembles, and its .ref pins the words so a "fix"
#      that let the block through by dropping its last word still fails
#   2-4. the three xfail sources overrun by exactly one word and must still be
#      rejected. They are separate sources because the check lives in three
#      places -- the directive rules, setVarsPC and setConstPC -- and am1
#      stops at the first error, so one source could only ever test one.
run_test rg35_bank_ceiling         "$SCRIPT_DIR/rg35_bank_ceiling.am1"

run_test rg35_xfail_table_over     "$SCRIPT_DIR/rg35_xfail_table_over.am1"     --xfail
run_test rg35_xfail_vars_over      "$SCRIPT_DIR/rg35_xfail_vars_over.am1"      --xfail
run_test rg35_xfail_consts_over    "$SCRIPT_DIR/rg35_xfail_consts_over.am1"    --xfail
echo ""

# Not a failure: an --xfail test never gets far enough to have words to
# compare, and rg34 is checked through its listing instead. It is printed so a
# new test cannot quietly join the suite with nothing but an exit code behind
# it -- which is how the suite came to be passing rg10_locals against a .ref
# that was two words short of the source.
if [ ${#NOREF[@]} -gt 0 ]; then
    echo "No .ref, exit status only: ${NOREF[*]}"
fi

echo "Results: $pass pass, $fail fail, $xfail expected-fail"
[ ${#FAILURES[@]} -gt 0 ] && echo "Failures: ${FAILURES[*]}"
[ $fail -eq 0 ]

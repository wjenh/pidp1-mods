#!/bin/bash
# run_regression.sh -- comprehensive am1 regression suite
# Usage: ./run_regression.sh [am1]
#   am1   the am1 to test, absolute or relative to where you run this.
#         Default: ../../am1 relative to this script, i.e. the am1 of the tree it sits in.
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

# Made absolute before the cd below: a relative path would pass the -x test
# from the caller's directory and then fail every source from the temp one.
AM1="$(cd "$(dirname "$AM1")" && pwd)/$(basename "$AM1")"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

pass=0; fail=0; xfail=0
declare -a FAILURES
declare -a NOREF

# Compare what am1 generated against the stored .ref.
#
# This is a SECOND invocation of am1 rather than a -T added to the test's own
# run: -T selects a different code generator, and that generator does not make
# every check the binary one does (rg30's duplicate 100/ origin is reported
# without -T and assembles silently under it). The plain run keeps every
# check; the -T run only supplies the word dump.
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

# --xmsg TEXT: an --xfail test must also print TEXT, so that it
# fails for the reason it exists and not for any other.  A bare --xfail
# passes on any error at all.  am1 prints an error and its "at line N, file F"
# on separate lines, so TEXT is looked for in the log joined into one line.
run_test() {
    local name=$1 src=$2 expect_fail=0 xmsg=""
    shift 2
    local flags=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --xfail) expect_fail=1; shift ;;
            --xmsg) xmsg="$2"; shift 2 ;;
            *) flags+=("$1"); shift ;;
        esac
    done

    "$AM1" "${flags[@]}" -I"$SCRIPT_DIR" "$src" > "$WORK/$name.log" 2>&1
    local rc=$?

    if [ "$expect_fail" -eq 1 ]; then
        if [ $rc -ne 0 ] && [ -n "$xmsg" ] && ! tr '\n' ' ' < "$WORK/$name.log" | grep -qF -- "$xmsg"; then
            echo "FAIL:  $name (failed, but without the message \"$xmsg\")"
            sed 's/^/       /' "$WORK/$name.log"
            fail=$((fail+1))
            FAILURES+=("$name")
        elif [ $rc -ne 0 ]; then
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

# rg32 -- constants-pool determinism: the pool layout must not depend on
# malloc addresses, so the same source assembles identically on every run
# under ASLR. Three checks, deliberately WITHOUT any setarch -R wrapper:
#   1. it assembles (run_test as usual)
#   2. two plain runs produce byte-identical .rim output
#   3. the -T dump matches the stored .ref (run_test) -- this pins the exact
#      pool layout, including the wildcard-ref-dedup-by-name behavior.
#      Regenerate the .ref only if the fingerprint scheme is deliberately
#      changed.
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
# rg33 -- placement of the automatically emitted constants and variables blocks:
# each lands in its own bank, in the order the parser assigned, at the address
# the parser assigned. Two checks, and both are needed:
#   1. the -T dump pins the addresses the parser assigns (run_test)
#   2. the loader block headers in the .rim pin where the binary generator
#      actually writes them. The -T dump comes from a different generator and
#      cannot show a block written to the wrong bank.
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

# rg34 -- pooled-literal labels in the listing. Literals are pooled BY VALUE,
# so two references whose values coincide share one pool word, and the listing
# must still label each reference with its own expression, not with whichever
# reference interned the slot first. The object code is the same either way,
# so nothing else in this suite would catch a wrong label.
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


# rg35 -- the last word of a bank. A block that ends exactly on 07777 leaves
# cur_pc at 010000, one PAST the last word it used, and must still be accepted:
# table, text, ascii, type340, variables and constants can all reach the top
# word of a bank, as a plain instruction can (the control leg rg35_bank_ceiling
# carries in bank 0). Four checks, because the bound must hold both ways:
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

# rg36 to rg43 -- the overwrite check covers every word bincodegen.c emits, so
# a collision on any of them is an error, not a silent exit 0. Seven xfail
# sources, one per case and one per direction, because am1 stops at the first
# error:
#   rg36/rg37  a word on a label's line, overwritten and overwriting
#   rg38/rg39  an element of an initialized table, both directions
#   rg40       ascii's NUL-padded last word
#   rg41/rg42  variables and constants landing on a labeled word: the word
#              beneath them must be marked for their check to see it
# rg43 is the -M leg: these words the author wrote follow -M, as an unlabeled
# word does (rg25), so it assembles and its .ref pins the words.
run_test rg36_xfail_over_label     "$SCRIPT_DIR/rg36_xfail_over_label.am1"     --xfail
run_test rg37_xfail_label_over     "$SCRIPT_DIR/rg37_xfail_label_over.am1"     --xfail
run_test rg38_xfail_tinit_over     "$SCRIPT_DIR/rg38_xfail_tinit_over.am1"     --xfail
run_test rg39_xfail_over_tinit     "$SCRIPT_DIR/rg39_xfail_over_tinit.am1"     --xfail
run_test rg40_xfail_ascii_pad      "$SCRIPT_DIR/rg40_xfail_ascii_pad.am1"      --xfail
run_test rg41_xfail_vars_over_label   "$SCRIPT_DIR/rg41_xfail_vars_over_label.am1"   --xfail
run_test rg42_xfail_consts_over_label "$SCRIPT_DIR/rg42_xfail_consts_over_label.am1" --xfail
run_test rg43_flag_M_label         "$SCRIPT_DIR/rg43_flag_M_label.am1"         -M

# % is modulo, there are no %name locals, and the directives are spelled %%.
# rg44 is a label written as %name, which is now a % with no left operand;
# rg45 is modulo, with and without blanks, its .ref derived by hand; rg46 is
# %% before a name that is not a directive, and the message names it.  A
# "jmp %c" is modulo, so no test pins an error for it.
run_test rg44_xfail_pct_label      "$SCRIPT_DIR/rg44_xfail_pct_label.am1"      --xfail --xmsg "syntax error at line 6,"
run_test rg45_modulo               "$SCRIPT_DIR/rg45_modulo.am1"
run_test rg46_xfail_pct_directive  "$SCRIPT_DIR/rg46_xfail_pct_directive.am1"  --xfail --xmsg "%%optimizer is not an am1 directive"
echo ""

# rg47 and rg48: "NNN/ word" is an origin and then a statement, not NNN
# divided by the word, so a source copied from a macro listing keeps its
# origins. rg47 is the line itself and the divides that must stay divides;
# rg48 is the same line after every directive that takes text or operands,
# because the lexer has to know a new statement has started.
#   1. rg47 and rg48 assemble and their .refs pin the words (run_test)
#   2. rg47 written with tabs, as macro sources are, and cpp turns a tab into a
#      blank, so it is assembled again with -n, which keeps them, and the two
#      .rim files must be identical. The tab check keeps that from going
#      vacuous if the tabs are ever edited out of the source.
run_test rg47_origin_line          "$SCRIPT_DIR/rg47_origin_line.am1"
run_test rg48_origin_after_directives "$SCRIPT_DIR/rg48_origin_after_directives.am1"

cp rg47_origin_line.rim rg47_cpp.rim
"$AM1" -n -I"$SCRIPT_DIR" "$SCRIPT_DIR/rg47_origin_line.am1" > "$WORK/rg47_n.log" 2>&1
if ! grep -q "/$(printf '\t')" "$SCRIPT_DIR/rg47_origin_line.am1"; then
    echo "FAIL:  rg47_no_cpp_tabs (rg47_origin_line.am1 has no tab after an origin, so the -n leg tests nothing)"
    fail=$((fail+1))
    FAILURES+=("rg47_no_cpp_tabs")
elif cmp -s rg47_cpp.rim rg47_origin_line.rim; then
    echo "PASS:  rg47_no_cpp_tabs"
    pass=$((pass+1))
else
    echo "FAIL:  rg47_no_cpp_tabs (assembling with -n, which keeps the tabs, differs from assembling with cpp)"
    sed 's/^/       /' "$WORK/rg47_n.log"
    fail=$((fail+1))
    FAILURES+=("rg47_no_cpp_tabs")
fi
echo ""

# rg49 to rg53: type340 text, octal escapes and the ~ operator.
# rg52: & ^ and ` in type340 text get the shift they need ahead of them.
# rg53: an octal escape takes no 8 or 9.
# rg49: a type340 escape is a character like any other: a backslash, a quote,
# \b, \s, \u and \0 each give their Type 340 code. The .ref is derived by hand
# from the Type 340 code tables, not from what am1 printed.
# rg50, rg51: "lac ~07" is the complement, the blanks in front of the ~ being
# the separator. -a makes it an addition.
#   1. the three assemble and their .refs pin the words (run_test)
#   2. rg50 with a tab, which cpp turns into a blank: assembled again with -n,
#      which keeps it, the two .rim files must be identical. The tab check keeps
#      that from going vacuous if the tab is ever edited out.
#   3. an unknown escape, "A\xB", is warned about once, keeps its character,
#      and -W=-type340 silences the warning
run_test rg49_type340_escapes       "$SCRIPT_DIR/rg49_type340_escapes.am1"
run_test rg50_complement_separator  "$SCRIPT_DIR/rg50_complement_separator.am1"
run_test rg51_complement_separator_a "$SCRIPT_DIR/rg51_complement_separator_a.am1" -a
run_test rg52_type340_shifts        "$SCRIPT_DIR/rg52_type340_shifts.am1"
run_test rg53_octal_escapes         "$SCRIPT_DIR/rg53_octal_escapes.am1"
run_test rg53_xfail_char_octal_8    "$SCRIPT_DIR/rg53_xfail_char_octal_8.am1"    --xfail

cp rg50_complement_separator.rim rg50_cpp.rim
"$AM1" -n -I"$SCRIPT_DIR" "$SCRIPT_DIR/rg50_complement_separator.am1" > "$WORK/rg50_n.log" 2>&1
if ! grep -q "lac$(printf '\t')~" "$SCRIPT_DIR/rg50_complement_separator.am1"; then
    echo "FAIL:  rg50_no_cpp_tabs (rg50_complement_separator.am1 has no tab before a ~, so the -n leg tests nothing)"
    fail=$((fail+1))
    FAILURES+=("rg50_no_cpp_tabs")
elif cmp -s rg50_cpp.rim rg50_complement_separator.rim; then
    echo "PASS:  rg50_no_cpp_tabs"
    pass=$((pass+1))
else
    echo "FAIL:  rg50_no_cpp_tabs (assembling with -n, which keeps the tabs, differs from assembling with cpp)"
    sed 's/^/       /' "$WORK/rg50_n.log"
    fail=$((fail+1))
    FAILURES+=("rg50_no_cpp_tabs")
fi

# An unknown escape warns and keeps its character. The source is written here,
# not kept beside rg49, because other test scripts assemble every rg*.am1
# and want no output on stderr. "A\xB" is A, lower shift, x (030), upper shift, B.
cat > "$WORK/rg49_unknown.am1" <<'UNKNOWN_END'
Test rg49: an unknown type340 escape
100/
    type340 "A\xB"
stop
UNKNOWN_END
printf '000100 013630\n000101 350237\n000000 000000\n' > "$WORK/rg49_unknown.ref"

"$AM1" -T "$WORK/rg49_unknown.am1" > "$WORK/rg49_w.log" 2>&1
cp rg49_unknown.dmp rg49_unknown_w.dmp
"$AM1" -T -W=-type340 "$WORK/rg49_unknown.am1" > "$WORK/rg49_nw.log" 2>&1
if [ "$(grep -c 'is not a type340 escape' "$WORK/rg49_w.log")" -eq 1 ] && \
   ! grep -q 'is not a type340 escape' "$WORK/rg49_nw.log" && \
   cmp -s rg49_unknown_w.dmp "$WORK/rg49_unknown.ref" && \
   cmp -s rg49_unknown.dmp "$WORK/rg49_unknown.ref"; then
    echo "PASS:  rg49_unknown_escape"
    pass=$((pass+1))
else
    echo "FAIL:  rg49_unknown_escape (want one warning by default and the words 013630 350237, no warning with -W=-type340)"
    sed 's/^/       /' "$WORK/rg49_w.log" "$WORK/rg49_nw.log"
    fail=$((fail+1))
    FAILURES+=("rg49_unknown_escape")
fi
echo ""

# Not a failure: an --xfail test never gets far enough to have words to
# compare, and rg34 is checked through its listing instead. It is printed so a
# new test cannot quietly join the suite with nothing but an exit code behind
# it.
if [ ${#NOREF[@]} -gt 0 ]; then
    echo "No .ref, exit status only: ${NOREF[*]}"
fi

echo "Results: $pass pass, $fail fail, $xfail expected-fail"
[ ${#FAILURES[@]} -gt 0 ] && echo "Failures: ${FAILURES[*]}"
[ $fail -eq 0 ]

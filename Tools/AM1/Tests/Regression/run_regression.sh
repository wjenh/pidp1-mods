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
#   1. each reference is labeled with what the source wrote
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
    echo "FAIL:  rg34_litpool_label_fidelity (a pooled literal is labeled with another reference's expression)"
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

# rg54: literals that apply an operator to a symbol not yet defined, which once
# shared one pool word and loaded the first one's value. The words its .ref pins
# were checked against the values written in its comments before it was made.
run_test rg54_litpool_forward       "$SCRIPT_DIR/rg54_litpool_forward.am1"
echo ""

# rg55: literals whose values are all known but whose pool key's number is not
# their value (-0, a negative operand, and under -a a blank), which once shared
# one pool word and loaded the first one's value. The words each .ref pins were
# checked against the values written in its comments before it was made.
run_test rg55_litpool_known         "$SCRIPT_DIR/rg55_litpool_known.am1"
run_test rg55_litpool_known_a       "$SCRIPT_DIR/rg55_litpool_known_a.am1"  -a
echo ""

# rg56: a keyword written as a label, at the start of its line, is reported as
# one, on its own line.
#   1. rg56 is start as a local label after a literal naming a later label,
#      once reported as that label undefined, at the literal's line
#   2. each of the six keywords lexed at the start of a line, as a label at
#      column 0 and, with a blank before the comma, indented
#   3. a keyword away from the start of its line: after a ';', as a label and
#      as a statement, in a local, addlocal and private list, and as an
#      operand. The first once assembled as a label, a local list took the
#      name silently, and the rest were reported as undefined at the end
#   4. the control: labels at column 0 that only begin with a keyword still
#      assemble
# The legs' sources are written here, not kept beside rg56, because other
# test scripts assemble every rg*.am1 not named _xfail_.
run_test rg56_xfail_keyword_label   "$SCRIPT_DIR/rg56_xfail_keyword_label.am1"  --xfail --xmsg "start is an am1 keyword and cannot be a label at line 7,"
for kw in start stop constants variables import export; do
    printf 'Test rg56: label %s\n100/\n%s, hlt\nstop\n' "$kw" "$kw" > "$WORK/rg56_$kw.am1"
    run_test rg56_$kw "$WORK/rg56_$kw.am1" --xfail --xmsg "$kw is an am1 keyword and cannot be a label at line 3,"
    printf 'Test rg56: label %s, indented\n100/\n    %s , hlt\nstop\n' "$kw" "$kw" > "$WORK/rg56_${kw}_ind.am1"
    run_test rg56_${kw}_indented "$WORK/rg56_${kw}_ind.am1" --xfail --xmsg "$kw is an am1 keyword and cannot be a label at line 3,"
done
rg56_away() {   # leg name, keyword, the line written on line 4
    printf 'Test rg56: %s away from the start of a line\n100/\nx, 0\n%s\n    hlt\nstart 100\n' "$2" "$3" > "$WORK/rg56_$1.am1"
    run_test rg56_$1 "$WORK/rg56_$1.am1" --xfail --xmsg "$2 is an am1 keyword, allowed only at the start of a line at line 4,"
}
rg56_away semi_label     start      '    hlt; start, law 7'
rg56_away semi_stmt      constants  '    hlt; constants'
rg56_away semi_stop      stop       '    hlt; stop'
rg56_away local          start      '    local start'
rg56_away local_list     import     '    local a, import, b'
rg56_away addlocal       variables  '    addlocal variables'
rg56_away private        export     '    private export'
rg56_away operand        stop       '    jmp stop'
printf 'Test rg56: labels that begin with a keyword\n100/\nstarter, jmp stopper\nstopper, lac constantsx\nexports, hlt\nconstantsx, 7\nstart 100\n' > "$WORK/rg56_prefix.am1"
"$AM1" -T -I"$SCRIPT_DIR" "$WORK/rg56_prefix.am1" > "$WORK/rg56_prefix.log" 2>&1
if [ -f rg56_prefix.dmp ] && [ "$(tr '\n' ' ' < rg56_prefix.dmp)" = "000100 600101 000101 200103 000102 760400 000103 000007 000100 000100 " ]; then
    echo "PASS:  rg56_prefix_labels"
    pass=$((pass+1))
else
    echo "FAIL:  rg56_prefix_labels (want the words 600101 200103 760400 000007, start 100)"
    sed 's/^/       /' "$WORK/rg56_prefix.log"
    [ -f rg56_prefix.dmp ] && sed 's/^/       /' rg56_prefix.dmp
    fail=$((fail+1))
    FAILURES+=("rg56_prefix_labels")
fi
echo ""

# rg57: indented names that only begin with a keyword, once split into the
# keyword and the rest, are names. Blank and tab indents, an indented
# constants and start among them, assembled with cpp and with -n, which keeps
# the tabs. The source is written here for the reason rg56's legs are.
printf 'Test rg57: indented names that begin with a keyword\n100/\n    starter, jmp stop_2\n    stop_2, lac constants1\n\tvariablesX, law 5\n    importer, hlt\n\texportAddr, 7\n\tconstants1, 3\n    constants\n    start 100\n' > "$WORK/rg57_prefix_indented.am1"
for leg in cpp n; do
    lflag=""; [ "$leg" = n ] && lflag=-n
    rm -f rg57_prefix_indented.dmp
    "$AM1" -T $lflag -I"$SCRIPT_DIR" "$WORK/rg57_prefix_indented.am1" > "$WORK/rg57_$leg.log" 2>&1
    if [ -f rg57_prefix_indented.dmp ] && [ "$(tr '\n' ' ' < rg57_prefix_indented.dmp)" = "000100 600101 000101 200105 000102 700005 000103 760400 000104 000007 000105 000003 000100 000100 " ]; then
        echo "PASS:  rg57_prefix_indented_$leg"
        pass=$((pass+1))
    else
        echo "FAIL:  rg57_prefix_indented_$leg (want the words 600101 200105 700005 760400 000007 000003, start 100)"
        sed 's/^/       /' "$WORK/rg57_$leg.log"
        [ -f rg57_prefix_indented.dmp ] && sed 's/^/       /' rg57_prefix_indented.dmp
        fail=$((fail+1))
        FAILURES+=("rg57_prefix_indented_$leg")
    fi
done
echo ""

# rg58: a bank wildcard, name:*, followed by another term, once a syntax error,
# in this bank and another, in a literal, through a macro and before a
# complement. The words its .ref pins are those of the same lines with each
# name:* parenthesized, assembled by am1 v5.5, which took that form.
run_test rg58_bank_wildtag          "$SCRIPT_DIR/rg58_bank_wildtag.am1"
echo ""

# rg59: start and banks. A bare name is the current bank's if that bank defines
# it, and otherwise must be defined in exactly one other bank; the start address
# carries the bank it was found in.
#   1. a label in one other bank, first used after the current one, once refused
#   2. a label in the current bank, nonzero, and in bank 0, whose start address
#      once had no bank
#   3. a label no bank defines, reported on the start line, once one past it
#   4. a label two other banks define, which start must qualify
run_test rg59_start_other_bank      "$SCRIPT_DIR/rg59_start_other_bank.am1"
run_test rg59_start_this_bank       "$SCRIPT_DIR/rg59_start_this_bank.am1"
run_test rg59_xfail_start_nowhere   "$SCRIPT_DIR/rg59_xfail_start_nowhere.am1"  --xfail --xmsg "start symbol go has no defined value in any bank at line 8,"
run_test rg59_xfail_start_ambiguous "$SCRIPT_DIR/rg59_xfail_start_ambiguous.am1"  --xfail --xmsg "start symbol go is defined in banks 0, 5 and not in bank 3; give its bank, go:N at line 12,"
echo ""

# rg60: an #error inside a #define block is refused with its own text in the
# message, which once said only "#error inside a #define block".
run_test rg60_xfail_define_error    "$SCRIPT_DIR/rg60_xfail_define_error.am1"  --xfail --xmsg "#error inside a #define block: the CHECK text"
echo ""

# rg61: comments on and after the start and stop lines, once a syntax error.
#   1. start with a // comment, from another bank, then // and C-style comment
#      lines and a blank line; the .ref pins the words of the uncommented program
#   2. the same for stop
#   3. code after the comment lines is still refused
run_test rg61_start_comment         "$SCRIPT_DIR/rg61_start_comment.am1"
run_test rg61_stop_comment          "$SCRIPT_DIR/rg61_stop_comment.am1"
run_test rg61_xfail_code_after_start "$SCRIPT_DIR/rg61_xfail_code_after_start.am1"  --xfail --xmsg "syntax error"
echo ""

# rg62 to rg67: assert, a check on assembled values, and -A, which ignores it.
# A true assert emits no word, so each true fixture's .ref holds the words of
# the same source with its assert lines left blank, and rg_same compares a
# build with the one of that blank-line copy. The false and refused legs are
# written here, for the reason rg56's are, or made from a true fixture by sed.
rg_result() {   # leg name, what went wrong, empty for a pass
    if [ -z "$2" ]; then
        echo "PASS:  $1"
        pass=$((pass+1))
    else
        echo "FAIL:  $1 ($2)"
        fail=$((fail+1))
        FAILURES+=("$1")
    fi
}
# A source and its blank-line copy, each built in its own directory with the
# flags given, must agree in every output named. "out" is stdout and stderr;
# "lst-" and "mac-" compare the listing and the macro1 file less their assert
# lines, and require one such line for each assert in the source.
rg_same() {     # leg name, source, outputs to compare, flags...
    local name=$1 src=$2 exts=$3 e a b n bad=""
    shift 3
    rm -rf "$WORK/$name.a" "$WORK/$name.b"
    mkdir -p "$WORK/$name.a" "$WORK/$name.b"
    cp "$src" "$WORK/$name.a/t.am1"
    sed 's/^assert .*$//' "$src" > "$WORK/$name.b/t.am1"
    n=$(grep -c '^assert ' "$src")
    if [ "$n" -eq 0 ]; then
        bad="no assert line to blank"
    elif ! (cd "$WORK/$name.a" && "$AM1" "$@" -I"$SCRIPT_DIR" t.am1 > t.out 2>&1) ||
         ! (cd "$WORK/$name.b" && "$AM1" "$@" -I"$SCRIPT_DIR" t.am1 > t.out 2>&1); then
        bad="a build failed"
        sed 's/^/       /' "$WORK/$name.a/t.out" "$WORK/$name.b/t.out"
    else
        for e in $exts; do
            a="$WORK/$name.a/t.${e%-}"
            b="$WORK/$name.b/t.${e%-}"
            case "$e" in
                lst-)
                    { [ "$(grep -c '^ *[0-9]*: [0-7]\{6\}        assert ' "$a")" -eq "$n" ] &&
                      grep -v '^ *[0-9]*: [0-7]\{6\}        assert ' "$a" | cmp -s - "$b"; } || bad="$bad $e"
                    ;;
                mac-)
                    { [ "$(grep -c '^/ assert ' "$a")" -eq "$n" ] &&
                      grep -v '^/ assert ' "$a" | cmp -s - "$b"; } || bad="$bad $e"
                    ;;
                *)
                    { [ -f "$a" ] && cmp -s "$a" "$b"; } || bad="$bad $e"
                    ;;
            esac
        done
        [ -n "$bad" ] && bad="differs from the blank-line copy in:$bad"
    fi
    rg_result "$name" "$bad"
}
# Each assert of a true fixture, its expression put under !(...), must fail on
# its own line with its own message.
rg_negate_all() {   # leg prefix, true fixture
    local n msg
    while IFS=: read -r n _; do
        msg=$(sed -n "${n}s/.*, \"\(.*\)\"\$/\1/p" "$2")
        sed "${n}s/^assert \(.*\), \"/assert !(\1), \"/" "$2" > "$WORK/$1_$n.am1"
        if cmp -s "$2" "$WORK/$1_$n.am1" || [ -z "$msg" ]; then
            rg_result "$1_$n" "line $n has no assert with a message to negate"
        else
            run_test "$1_$n" "$WORK/$1_$n.am1" --xfail --xmsg "assert failed: $msg at line $n,"
        fi
    done < <(grep -n '^assert ' "$2")
}

# rg62: the nine operators. Each holds where written and gives 0 under !, -0
# equals +0, a word with its sign bit set is negative, an address above
# 0100000 is positive, and the priorities are C's, all below |. Then each
# assert negated, and a false one of each operator on its own, which writes no
# tape; under -A that one assembles to the program without its line.
run_test rg62_assert_operators      "$SCRIPT_DIR/rg62_assert_operators.am1"
rg_negate_all rg62_negated "$SCRIPT_DIR/rg62_assert_operators.am1"
rg62_op() {     # operator's name, a false expression with it
    printf 'Test rg62: %s, false\n100/\nx, 0\nassert %s, "%s"\nstart x\n' "$1" "$2" "$1" > "$WORK/rg62_$1.am1"
    run_test rg62_false_$1 "$WORK/rg62_$1.am1" --xfail --xmsg "assert failed: $1 at line 4,"
}
rg62_op eq  '1 == 2'
rg62_op ne  '0777777 != 0'
rg62_op lt  '0 < -3'
rg62_op le  '1 <= -3'
rg62_op gt  '-1 > 2'
rg62_op ge  '-1 >= 0'
rg62_op and '1 && 0'
rg62_op or  '0 || 0'
rg62_op not '!1'
if [ ! -f rg62_eq.rim ] && [ -f rg62_assert_operators.rim ]; then
    rg_result rg62_false_no_tape ""
else
    rg_result rg62_false_no_tape "a failed assert left rg62_eq.rim, or the true fixture wrote no .rim"
fi
rg_same rg62_false_eq_A "$WORK/rg62_eq.am1" "rim sym lst mac out" -A -d -m -b
echo ""

# rg63: the values an assert sees: a forward reference, '.' in bank 0, after a
# constant pool and in bank 3, a variable, thisbank, and names in another bank,
# by name:* and name:N. Each negated must fail, as must an undefined name, on
# the assert's line; -l, -m, -S, the .sym and the .map are those of the source
# without the assert lines, the listing and -m less those lines.
run_test rg63_assert_values         "$SCRIPT_DIR/rg63_assert_values.am1"
rg_negate_all rg63_negated "$SCRIPT_DIR/rg63_assert_values.am1"
rg63_name() {   # leg name, what replaces fwd on line 5, the message
    sed "5s/^assert fwd /assert $2 /" "$SCRIPT_DIR/rg63_assert_values.am1" > "$WORK/rg63_$1.am1"
    run_test rg63_$1 "$WORK/rg63_$1.am1" --xfail --xmsg "$3 at line 5,"
}
rg63_name undefined      nowhere    "symbol nowhere has no defined value"
rg63_name undefined_bank nowhere:3  "symbol nowhere in bank 3 has no defined value"
rg63_name undefined_wild nowhere:\\* "wildcarded symbol 'nowhere' cannot be found"
rg_same rg63_outputs "$SCRIPT_DIR/rg63_assert_values.am1" "rim sym map out lst- mac-" -d -S -m -b
# A forward reference to a label made under %%forcelocal, which goes into the
# local scope alone, once reported as having no defined value.
printf 'Test rg63: a forward reference to a %%%%forcelocal label\n100/\ngo, hlt\n    local\n%%%%forcelocal\nassert c == 0101, "forced local"\nc, 0\n%%%%forcelocal\n    endlocal\nstart go\n' > "$WORK/rg63_forcelocal.am1"
rg_same rg63_forcelocal "$WORK/rg63_forcelocal.am1" "rim sym" -s
rg_negate_all rg63_forcelocal_negated "$WORK/rg63_forcelocal.am1"
echo ""

# rg64: where a failure is reported: in an included file, at its own line in
# that file; in a macro body, at the line that expanded it; with no message,
# "assert failed" alone.
printf 'Test rg64: an assert in an included file\n100/\nx, 0\n#include "rg64_assert.ah"\nstart x\n' > "$WORK/rg64_include.am1"
run_test rg64_include "$WORK/rg64_include.am1" --xfail --xmsg "assert failed: from the include at line 3, file $SCRIPT_DIR/rg64_assert.ah"
printf 'Test rg64: an assert in a macro body\n#define CHECK(e) assert e, "macro check"\n100/\nx, 0\n    hlt\nCHECK(x == 0101)\nstart x\n' > "$WORK/rg64_macro.am1"
run_test rg64_macro "$WORK/rg64_macro.am1" --xfail --xmsg "assert failed: macro check at line 6,"
printf 'Test rg64: an assert with no message\n100/\nx, 0\nassert x - x\nstart x\n' > "$WORK/rg64_nomsg.am1"
run_test rg64_nomsg "$WORK/rg64_nomsg.am1" --xfail --xmsg "assert failed at line 4,"
echo ""

# rg65: assert is a keyword. As a label, at column 0 and indented, and as an
# operand it is reported as one; a name that only begins with it is a name.
# After a ';' it is an assert, checked on its own line (rg68 has more). The new operators are refused outside an assert, as is a constant
# inside one; a syntax error in one and one inside an overlay are refused, and
# all of these still are under -A.
run_test rg65_assert_prefix         "$SCRIPT_DIR/rg65_assert_prefix.am1"
rg65_leg() {    # leg name, the line written on line 4, the message
    printf 'Test rg65: %s\n100/\nx, 0\n%s\n    hlt\nstart x\n' "$1" "$2" > "$WORK/rg65_$1.am1"
    run_test rg65_$1 "$WORK/rg65_$1.am1" --xfail --xmsg "$3 at line 4,"
    run_test rg65_${1}_A "$WORK/rg65_$1.am1" -A --xfail --xmsg "$3 at line 4,"
}
rg65_leg label      'assert, hlt'          "assert is an am1 keyword and cannot be a label"
rg65_leg label_ind  '    assert , hlt'     "assert is an am1 keyword and cannot be a label"
rg65_leg operand    '    lac assert'       "assert is an am1 keyword, allowed only at the start of a line"
rg65_leg constant   'assert [5] == 5'      "a constant cannot be used in an assert, its word would be added to the program"
rg65_leg syntax     'assert x == == 1'     "syntax error"
rg65_leg op_eq      '    lac x == 1'       "== is a comparison or logical operator, allowed only in an assert"
rg65_leg op_ne      '    lac x != 1'       "!= is a comparison or logical operator, allowed only in an assert"
rg65_leg op_lt      '    hlt; lac x < 1'   "< is a comparison or logical operator, allowed only in an assert"
rg65_leg op_le      '    lac x <= 1'       "<= is a comparison or logical operator, allowed only in an assert"
rg65_leg op_gt      '    lac x > 1'        "> is a comparison or logical operator, allowed only in an assert"
rg65_leg op_ge      '    lac x >= 1'       ">= is a comparison or logical operator, allowed only in an assert"
rg65_leg op_and     '    lac x && 1'       "&& is a comparison or logical operator, allowed only in an assert"
rg65_leg op_or      '    lac x || 1'       "|| is a comparison or logical operator, allowed only in an assert"
rg65_leg op_not     '    lac !x'           "! is a comparison or logical operator, allowed only in an assert"
printf 'Test rg65: an assert in an overlay\n%%%%drumreserve 0-17\nbank 2\n%%%%window w1, 2000, 2777\n%%%%overlay edit, w1\n        export editRun\neditRun, hlt\nassert editRun == 02000\n%%%%endoverlay\nbank 0\n100/\ngo,     hlt\n%%%%ovtable\n        start go\n' > "$WORK/rg65_overlay.am1"
run_test rg65_overlay   "$WORK/rg65_overlay.am1" --xfail --xmsg "an assert cannot be used inside overlay edit, opened at line 5"
run_test rg65_overlay_A "$WORK/rg65_overlay.am1" -A --xfail --xmsg "an assert cannot be used inside overlay edit, opened at line 5"
echo ""

# rg66: the optimizer. Asserts that hold at every level, one of them on '.'
# after the deleted word, give the words of the source without them at -O1
# and -O2, which do delete it; -O=source writes them back, and the file it
# writes assembles to the same tape. A size assert that -O1 and -O2 break
# fails "after optimization", leaving no .rim and no .opt; under -O it holds.
run_test rg66_assert_optimize       "$SCRIPT_DIR/rg66_assert_optimize.am1"
rg_same rg66_optimize_O1 "$SCRIPT_DIR/rg66_assert_optimize.am1" "rim" -O1
rg_same rg66_optimize_O2 "$SCRIPT_DIR/rg66_assert_optimize.am1" "rim" -O2
if [ -f rg66_assert_optimize.rim ] && ! cmp -s rg66_assert_optimize.rim "$WORK/rg66_optimize_O1.a/t.rim"; then
    rg_result rg66_optimize_deletes ""
else
    rg_result rg66_optimize_deletes "-O1 left the tape unchanged, so nothing was relaid out"
fi
mkdir -p "$WORK/rg66_src"
cp "$SCRIPT_DIR/rg66_assert_optimize.am1" "$WORK/rg66_src/t.am1"
if (cd "$WORK/rg66_src" && "$AM1" -O1 -O=source t.am1 > o.log 2>&1 && mkdir w && cp t.opt.am1 w/ &&
    cd w && "$AM1" t.opt.am1 >> ../o.log 2>&1) &&
   [ "$(grep -c '^assert ' "$WORK/rg66_src/t.opt.am1")" -eq 3 ] &&
   cmp -s "$WORK/rg66_src/w/t.opt.rim" "$WORK/rg66_optimize_O1.a/t.rim"; then
    rg_result rg66_optimize_source ""
else
    rg_result rg66_optimize_source "-O=source lost an assert, or its file assembles to another tape"
fi
run_test rg66_assert_shrinks        "$SCRIPT_DIR/rg66_assert_shrinks.am1"
rg_same rg66_shrinks_O "$SCRIPT_DIR/rg66_assert_shrinks.am1" "rim" -O
# Built from a copy in the work directory, where its outputs go, so that the
# check for a .rim or .opt left behind looks where -O would write them; the
# -O build just before shows that it does.
cp "$SCRIPT_DIR/rg66_assert_shrinks.am1" "$WORK/rg66_shrinks.am1"
for lvl in O1 O2; do
    rm -f rg66_shrinks.rim rg66_shrinks.opt
    "$AM1" -O "$WORK/rg66_shrinks.am1" > /dev/null 2>&1
    if [ ! -f rg66_shrinks.rim ] || [ ! -f rg66_shrinks.opt ]; then
        rg_result rg66_shrinks_${lvl}_no_output "-O wrote no .rim or .opt here, so their absence below proves nothing"
        continue
    fi
    rm -f rg66_shrinks.rim rg66_shrinks.opt
    run_test rg66_shrinks_$lvl "$WORK/rg66_shrinks.am1" -$lvl --xfail --xmsg "assert failed after optimization: three words to end at line 11,"
    if [ -e rg66_shrinks.rim ] || [ -e rg66_shrinks.opt ]; then
        rg_result rg66_shrinks_${lvl}_no_output "the failed build left a .rim or a .opt"
    else
        rg_result rg66_shrinks_${lvl}_no_output ""
    fi
done
echo ""

# rg67: -A. A false assert and one naming an undefined symbol assemble, to the
# .ref's words and to every output of the source without them, printing
# nothing; without -A the source fails.
run_test rg67_assert_ignored        "$SCRIPT_DIR/rg67_assert_ignored.am1" -A
run_test rg67_assert_checked        "$SCRIPT_DIR/rg67_assert_ignored.am1" --xfail --xmsg "assert failed: false, ignored under -A at line 7,"
rg_same rg67_ignored_outputs "$SCRIPT_DIR/rg67_assert_ignored.am1" "rim sym map lst mac out" -A -d -S -m -b
rm -f rg67_assert_ignored.rim
"$AM1" -A "$SCRIPT_DIR/rg67_assert_ignored.am1" > "$WORK/rg67_silent.log" 2>&1
if [ ! -s "$WORK/rg67_silent.log" ] && [ -f rg67_assert_ignored.rim ]; then
    rg_result rg67_ignored_silent ""
else
    rg_result rg67_ignored_silent "-A printed something, or wrote no tape"
fi
echo ""

# rg68: an assert after a ';', so on a block macro's later lines, where the
# preprocessor joins them with ';'. The true fixture's asserts hold, and its
# rows sit ROWW apart; each leg below fails at its own assert's line, the one
# that used the macro or the one-line form's. The other line-start keywords
# are still refused after a ';'.
run_test rg68_block_assert          "$SCRIPT_DIR/rg68_block_assert.am1"
sed '20s/"Title"/"A title much too long"/' "$SCRIPT_DIR/rg68_block_assert.am1" > "$WORK/rg68_long.am1"
run_test rg68_long "$WORK/rg68_long.am1" --xfail --xmsg "assert failed: label too long at line 20,"
sed '22s/assert w == .*, "where"/assert w == 0, "where"/' "$SCRIPT_DIR/rg68_block_assert.am1" > "$WORK/rg68_oneline.am1"
run_test rg68_oneline "$WORK/rg68_oneline.am1" --xfail --xmsg "assert failed: where at line 22,"
printf 'Test rg68: after a semi\n100/\nx, 0\n    hlt; assert x == 0101, "semi"\n    hlt\nstart x\n' > "$WORK/rg68_semi.am1"
run_test rg68_semi "$WORK/rg68_semi.am1" --xfail --xmsg "assert failed: semi at line 4,"
rg68_kw() {     # leg name, the line written on line 4, the keyword
    printf 'Test rg68: %s\n100/\nx, 0\n%s\n    hlt\nstart x\n' "$1" "$2" > "$WORK/rg68_$1.am1"
    run_test rg68_$1 "$WORK/rg68_$1.am1" --xfail --xmsg "$3 is an am1 keyword, allowed only at the start of a line at line 4,"
}
rg68_kw start     '    0; start 100'  start
rg68_kw constants '    0; constants'  constants
echo ""

# rg69: a table's count. 0 adds no word and a row that fits exactly takes
# none, and the row's type340, after a ';' in the expansion, is listed at the
# line that used the macro. A negative count, -0 (0777777) among them, is its
# own error on the table's line, and writes no tape; so is a padded row too
# long for its block. (-0 written as such is +0, as rg62 shows.)
run_test rg69_table_zero            "$SCRIPT_DIR/rg69_table_zero.am1"
rm -rf "$WORK/rg69_lst" && mkdir "$WORK/rg69_lst"
if (cd "$WORK/rg69_lst" && cp "$SCRIPT_DIR/rg69_table_zero.am1" t.am1 && "$AM1" -l t.am1 > t.out 2>&1) &&
   [ "$(grep -c '^ *[0-9]*: 0001[0-7][0-7] 000000  [0-7]\{6\}$' "$WORK/rg69_lst/t.lst")" -gt 0 ]; then
    # Each row's type340 words, the lines after its "Type 340 table", carry
    # the line of the label just before them, the macro's use.
    bad=$(awk '/^ *[0-9]+: [0-7]+ [0-7]+ [a-z]+, 0;$/ { use = $1 }
               /^\/\/ Type 340 table/ { t = 1; next }
               /^\/\/ End/ { t = 0 }
               t && $1 != use { print }' "$WORK/rg69_lst/t.lst")
    rg_result rg69_type340_line "${bad:+a type340 word is listed off its macro use: $bad}"
else
    rg_result rg69_type340_line "the listing was not written, or has no type340 words"
fi
rg69_neg() {    # leg name, the line written on line 4, the count shown
    printf 'Test rg69: %s\n100/\nx, 0\n%s\n    hlt\nstart x\n' "$1" "$2" > "$WORK/rg69_$1.am1"
    rm -f "rg69_$1.rim"
    run_test rg69_$1 "$WORK/rg69_$1.am1" --xfail --xmsg "table count is negative ($3) at line 4,"
    if [ -e "rg69_$1.rim" ]; then
        rg_result rg69_${1}_no_tape "the failed build left a .rim"
    else
        rg_result rg69_${1}_no_tape ""
    fi
}
rg69_neg minus2   '    table 3 - 5, 0'  -2
rg69_neg minus0   '    table 0777777'   -0
sed '18s/"Truss >"/"A row much too long for it"/' "$SCRIPT_DIR/rg68_block_assert.am1" > "$WORK/rg69_row.am1"
run_test rg69_row "$WORK/rg69_row.am1" --xfail --xmsg "table count is negative (-5) at line 18,"
echo ""

# rg70: the line a message names. A syntax error found at a line's end, with a
# comment, after a ';', on a file's last line, in an include, at an include's
# last line, in a block macro and before a blank line is on its own line and
# file; one in mid-line, plain and after a ';', always was. An undefined name,
# plain, local, cross-bank and in an include, is reported at its reference, in
# its file, from the binary and the macro1 back ends.
rg70_case() {   # leg name, body (lines 3 on), the message, flags...
    local name=$1 body=$2 msg=$3
    shift 3
    printf 'Test rg70: %s\n100/\n%b    hlt\nstart 100\n' "$name" "$body" > "$WORK/rg70_$name.am1"
    run_test rg70_$name "$WORK/rg70_$name.am1" "$@" --xfail --xmsg "$msg"
}
printf '    0\n    lac 1 +\n    0\n' > "$WORK/rg70_inc.ah"
printf '    0\n    lac 1 +\n' > "$WORK/rg70_inclast.ah"
printf '    0\n    lac nosuch\n' > "$WORK/rg70_incsym.ah"
rg70_case eol          '    lac 1 +\n'                 "syntax error at line 3, file $WORK/rg70_eol.am1"
rg70_case eol_comment  '    lac 1 + // a comment\n'   "syntax error at line 3, file $WORK/rg70_eol_comment.am1"
rg70_case eol_semi     '    0; lac 1 +\n'              "syntax error at line 3, file $WORK/rg70_eol_semi.am1"
rg70_case mid          '    lac ) 1\n'                 "syntax error at line 3, file $WORK/rg70_mid.am1"
rg70_case mid_semi     '    0; lac ) 1\n'              "syntax error at line 3, file $WORK/rg70_mid_semi.am1"
rg70_case eol_gap      '    lac 1 +\n\n// note\n'      "syntax error at line 3, file $WORK/rg70_eol_gap.am1"
rg70_case include      '#include "rg70_inc.ah"\n'     "syntax error at line 2, file $WORK/rg70_inc.ah"
rg70_case include_last '#include "rg70_inclast.ah"\n' "syntax error at line 2, file $WORK/rg70_inclast.ah"
printf 'Test rg70: block\n#define B()\n    0\n    lac 1 +\n#enddef\n100/\n    B()\n    hlt\nstart 100\n' > "$WORK/rg70_block.am1"
run_test rg70_block "$WORK/rg70_block.am1" --xfail --xmsg "syntax error at line 7, file $WORK/rg70_block.am1"
printf 'Test rg70: last line\n100/\n    hlt\nstart 100 +\n' > "$WORK/rg70_lastline.am1"
run_test rg70_lastline "$WORK/rg70_lastline.am1" --xfail --xmsg "syntax error at line 4, file $WORK/rg70_lastline.am1"
for be in bin mac; do
    flags=()
    [ "$be" = mac ] && flags=(-m)
    rg70_case undef_$be       '    lac nosuch\n    0\n'               "symbol nosuch has no defined value at line 3, file $WORK/rg70_undef_$be.am1" "${flags[@]}"
    rg70_case local_$be       '    local q\n    lac q\n    endlocal\n' "local symbol q has no defined value at line 4, file $WORK/rg70_local_$be.am1" "${flags[@]}"
    rg70_case incsym_$be      '#include "rg70_incsym.ah"\n'          "symbol nosuch has no defined value at line 2, file $WORK/rg70_incsym.ah" "${flags[@]}"
    rg70_case xbank_$be       '    lac nosuch:3\n'                    "symbol nosuch in bank 3 has no defined value at line 3, file $WORK/rg70_xbank_$be.am1" "${flags[@]}"
done
echo ""

# rg71: -m refuses an undefined name:N, in another bank, its own and an
# include, as the binary back end does; a defined one still assembles. A
# failed run removes every output it had written: -m -b's .mac, its message
# given once, and -O's report. Each failure's twin that assembles leaves
# those same files, so the check that they are gone can fail.
printf '    0\n    lac nosuch:3\n' > "$WORK/rg71_incbref.ah"
rg71_case() {   # leg name, body (lines 3 on), the message, flags...
    local name=$1 body=$2 msg=$3
    shift 3
    printf 'Test rg71: %s\n100/\n%b    hlt\nstart 100\n' "$name" "$body" > "$WORK/rg71_$name.am1"
    run_test rg71_$name "$WORK/rg71_$name.am1" "$@" --xfail --xmsg "$msg"
}
rg71_case own_mac  '    lac nosuch:0\n'                "symbol nosuch in bank 0 has no defined value at line 3, file $WORK/rg71_own_mac.am1" -m
rg71_case inc_mac  '#include "rg71_incbref.ah"\n'      "symbol nosuch in bank 3 has no defined value at line 2, file $WORK/rg71_incbref.ah" -m
printf 'Test rg71: defined\n100/\n    lac x:1\n    hlt\nbank 1\n100/\nx, 0\nstart 100\n' > "$WORK/rg71_defined.am1"
run_test rg71_defined_mac "$WORK/rg71_defined.am1" -m
rg71_left() {   # leg name, body (lines 3 on), the exit wanted, the outputs to look for, flags...
    local name=$1 body=$2 want=$3 exts=$4 e rc bad=""
    shift 4
    rm -rf "$WORK/rg71_$name" && mkdir "$WORK/rg71_$name"
    printf 'Test rg71: %s\n100/\n%b    hlt\nstart 100\n' "$name" "$body" > "$WORK/rg71_$name/t.am1"
    (cd "$WORK/rg71_$name" && "$AM1" "$@" t.am1 > t.out 2>&1)
    rc=$?
    if [ "$want" = 0 ]; then
        [ $rc -eq 0 ] || bad="exit $rc"
        for e in $exts; do
            [ -e "$WORK/rg71_$name/t.$e" ] || bad="$bad, no .$e"
        done
    else
        [ $rc -ne 0 ] || bad="exit 0"
        for e in $exts; do
            [ -e "$WORK/rg71_$name/t.$e" ] && bad="$bad, .$e left"
        done
        [ "$(grep -c 'has no defined value\|would be overwritten' "$WORK/rg71_$name/t.out")" -eq 1 ] ||
            bad="$bad, not one message"
    fi
    rg_result rg71_$name "${bad#, }"
}
rg71_left mb_ok      '    lac x:1\nbank 1\n100/\nx, 0\nbank 0\n' 0 "mac rim" -m -b
rg71_left mb_undef   '    lac nosuch:3\n'                         1 "mac rim" -m -b
rg71_left mb_over    '    0\n100/\n    1\n'                        1 "mac rim" -m -b
rg71_left opt_ok     '    0\n'                                    0 "opt rim" -O -b
rg71_left opt_over   '    0\n100/\n    1\n'                        1 "opt rim" -O -b
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

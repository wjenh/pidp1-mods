# am1: bank-ceiling bound check and diagnostic line numbers

09-Sep-2026. Built and measured in a sandbox scratch copy of `Tools/AM1`; the
sandbox `Tools/AM1` tree itself was left untouched. Unpack from the repo root:
paths inside are `Tools/AM1/...`. `am1-pcbound-lineno.diff` is the same change
as a unified diff for the eight modified files.

`Optimizer/AM1` is deliberately NOT included -- it carries the same eleven
`checkPCBound` call sites and the same `lexer.l` rule, and diverges until you
choose otherwise.

## 1. A block directive could not use the last word of a bank

`checkPCBound()` rejects `addr >= BANKSIZE`, so it wants an address. Every
directive that reserves a BLOCK advanced `cur_pc` and then handed it the
result -- which is one PAST the last word the block used. A block ending
exactly on 07777 left `cur_pc` at 010000 and was rejected.

Measured, bank 0, before the change:

| source | result |
|---|---|
| `07770/` `table 07` (ends 07776) | assembles |
| `07770/` `table 010` (ends 07777) | **rejected** |
| `07770/` `table 011` (ends 010000) | rejected, correctly |
| `07777/` `hlt` | assembles |

That last row is the control leg: an ordinary instruction reaches 07777, so it
is the post-increment check that lost the word, not the address. The same
failure was measured for `text`, `ascii`, `type340`, the labelled `text` form,
`variables` (via `setVarsPC`) and `constants` (via `setConstPC`).

Fixed at eleven sites in `parser.y`: each now checks the LAST address the
block used. `one_stmt: expr` ("Code") already checked before its increment and
is unchanged in that respect.

Genuine overruns are still rejected -- verified for `table`, `variables`,
`constants` and an over-long `text`.

## 2. Diagnostic line numbers

Two independent causes, both measured against sources whose true line was
known.

**(a) Parser lookahead.** `lineno` is advanced by the lexer when the statement
terminator is scanned. Whether a rule's action sees the old or the new value
depends on whether bison read that terminator as lookahead before reducing --
that is, on whether the state has a default reduction. From `y.output`:

  - state 70 `ASCII STRING .`, 71 `TYPE340 T340STRING .`, 28 `TEXT .`,
    169/170 (the labelTrailer forms): `$default reduce`, no lookahead read,
    `lineno` still correct.
  - state 69 `TABLE simple_expr .` and 155 `TABLE ... LOCATION simple_expr .`:
    both carry shift actions, so the lookahead MUST be read, and `lineno` has
    already moved on.

Measured: a `table` on line 5 was reported at line 6; `text`/`ascii`/`type340`
on line 5 were reported correctly. `one_stmt: expr` is a default-reduce state
but is reached through `expr: simple_expr`, which is not, so "Code" was also
one high -- measured at line 7 for a true line 6.

Fixed by reporting against the line the NODE recorded. `newnode()` already
normalises for the lookahead, so `$$->lineNo` is the line the user wrote no
matter which way the state went, and no site has to reason about bison tables.
The "Code" action now builds its node before the check so it has one to use.

**(b) A phantom newline after every C-style comment.** The `<CCOMMENT>"*/"`
rule does `unput('\n')`. When the `*/` ends a line, the pushed-back newline AND
the real one both reach the terminator rule, so `lineno` gains one. It is
cumulative and monotonic -- with one, two and three block comments ahead of it,
the same error was reported 1, 2 and 3 lines high. That is why the drift looks
intermittent: it depends on how many block comments precede the error.

Fixed with the idiom the `//` rule already uses for its own pushback:
`noIncrement = true` before the `unput`.

`rg02_comments` in the regression suite has carried this all along -- its
listing numbered `cla` as line 7 when the source has it on line 6. The suite
never caught it because the `.ref` compares words, not line numbers.

**Not affected, and measured to be sure:** cpp `#line` directives, both inside
an include and after returning from one; `;`-separated statements; `//`
comments. All were already correct and still are.

All fifteen line-number probes now report the true line.

## 3. The dead `lineNo` argument

`checkPCBound()` took a `lineNo` and passed it to `verror()` as a trailing
argument the format string never consumed; `verror()` reports the global
`lineno` regardless. Added `verrorl(int lineno, ...)` beside the existing
`vwarnl()`, which already had exactly this shape, and `checkPCBound()` now uses
it. This is what lets fix 2(a) work.

## 4. rg35

`rg35_bank_ceiling.am1` puts a block of each kind at the top of its own bank,
all eight ending exactly on 07777, with a plain instruction at 07777 in bank 0
as the control leg. The `.ref` pins the words, so a "fix" that admitted the
block by dropping its last word still fails.

Three `--xfail` sources overrun by exactly one word. They are separate files
because the check lives in three places -- the directive rules, `setVarsPC` and
`setConstPC` -- and am1 stops at the first error.

The test is not vacuous: against the current `Tools/AM1/am1`,
`rg35_bank_ceiling` FAILS (65 pass, 1 fail); against the fixed binary it
passes (67 pass, 0 fail, 6 expected-fail).

`refs_check.sh`, `rules_check.sh`, `flow_check.sh` and `decoder_check.sh` each
sweep every `rg*.am1` and skipped the xfail sources by the enumerated list
`rg29_*|rg30_*|rg31_*`. That list would have to grow for every future xfail, so
it is now `*_xfail_*`, which matches the same three files and the new ones.

## Test results, fixed binary

Regression 67 pass / 0 fail / 6 xfail (was 65/0/3), Multibank 5/0,
References 34/0, Rules 66/0, Flow 35/0, Decoder 34/0.

## byte_identity: three EXPECTED failures, please read

`Tests/Optimizer/byte_identity.sh` builds its reference from the frozen
`Tools/AM1/PreOptimizer` sources, so any deliberate non-optimizer change to am1
shows up there as a divergence. After this change it reports 35 pass, 3 fail:

  - `rg02_comments` -- listing line numbers, `cla` now 6 instead of 7. The new
    number is the correct one; see 2(b).
  - `rg35_bank_ceiling` -- the reference rejects it, the new binary assembles
    it. That is the fix.
  - `rg35_xfail_table_over` -- both reject it; the reference says line 9, the
    new binary says line 8. Line 8 is where the `table` is.

None is an optimizer regression. Deciding what to do about them -- refresh
`PreOptimizer`, add a skip list, or accept them -- is yours; nothing here
touches `byte_identity.sh` or `PreOptimizer`.

## Not done

  - `Optimizer/AM1` -- same eleven sites, same lexer rule, left to diverge.
  - No test covers the line numbers themselves. The runner has no mechanism for
    checking a diagnostic's text, and building one was not in scope; the xfail
    sources do capture stderr, so a check could be added cheaply if you want.
  - Cosmetic, spotted in a listing and not touched: a `;`-terminated statement
    emits a stray `;` on its own line, and an auto-emitted constants block's
    header comment prints with no line number.

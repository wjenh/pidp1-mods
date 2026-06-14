# AM1 Assembler for the PDP-1 — Session Notes

Carried over from a Claude Project chat ("Hardware emulation program training"),
prior to migrating to a Linux-based Claude client. The `am1-pdp1-assembler.skill`
file in this folder is the primary distilled artifact — load it into the new
project. The notes below cover working knowledge built up beyond the skill.

## Baseline knowledge established

- Full read of the am1 assembler documentation: syntax, directives, operators,
  number formats, bank/cross-bank references, locals, variables, constants,
  tables, text/ascii/type340 directives, and the full reserved symbol/opcode
  table, including PDP-1D extension opcodes (lia, lai, lsw/swp, cmi, sni, szi,
  scf, sci, ifi, iif, ida).
- The emulator now executes PDP-1D instructions natively (not just assembles
  them).

## Hardware/peripheral docs read and understood

- **Type 340 display**: vector/point/character/increment/subroutine commands,
  IOTs (dla/drs/dcf/etc.)
- **BBN timesharing clock**: IOT 32/2032/2132 — rck, cls, cct
- **Type 23 parallel drum**: IOT 61/62/63 — dia/dba, dwc, dcl, plus dra/dss
- **Type 33 symbol generator**: sdb, glf, gsp, gpl, gpr, gcf for 5x7 character
  drawing
- **Type 62 / Type 64 line printers**: prl/flb/slp/lpf/lpm vs lpc/lpb/pas/lpf/lpm
- **DCS2**: socket-based multi-channel serial I/O on IOT 22 with all subcommands

Relevant am1 include files: `memory.ah`, `TYPE340/type340defs.ah`,
`CLOCK/clockdefs.mh`, `LPT/type62defs.ah`, `LPT/type64defs.ah`.

## Worked examples

- Wrote a 32-bit counter program (start addr 100 octal) treating AC+IO as a
  combined 32-bit register, incrementing in a loop.
- **Bug found and fixed**: original version used `idx low` followed by an
  unconditional `jmp` to detect carry — but `idx` does not itself skip. Fixed
  by using `isp low` (increment and skip if positive) to correctly detect the
  no-carry case, removing the now-unnecessary `jmp`/label.
  - **Key lesson**: `idx` (index, no skip) vs `isp` (index and skip if
    positive) — don't confuse these when checking for carry/wraparound.

## Reviewed: rotate.am1 (existing complex program)

A bank-1 resident scheduler that round-robins through six programs stored on
six drum tracks, swapping the running program into bank 0 every minute via the
BBN clock's 1-minute interrupt. Key points understood:

- Front-panel switches set initial drum track numbers (packed 6 bits each into
  18-bit words via `lat`/`dac`).
- `resume` rotates a 36-bit AC:IO combined register right by 6 bits to cycle
  through the six track numbers.
- Selected track number is masked/shifted into the drum command field, OR'd
  with `drmrd`, issued via `dia`; `cli` + `dwc`/`dcl` triggers a full 4096-word
  transfer overwriting bank 0.
- `farmemcpy` is used to patch bank 0 afterward: copies a 4-word boot template
  and the `isr`/`rti`/`go` glue block from bank 1 into bank 0 at fixed offsets
  (7751 region), so the same plain-addressed code works in either bank.
- Handoff sequence: `esm` (enable SBS), `cls` (arm clock interrupt), `eem`,
  `farjmp(0)` → bank 0 location 0 → `lem`/`jmp go` → checks `startaddr` (7773).
- One minute later, clock interrupt → `isr` → `rti` → `eem` + far-jump back to
  `resume`, restarting the cycle.
- Notable technique: sharing one piece of glue code (`go`/`isr`/`rti`) between
  banks by copying it verbatim with preserved relative offsets to both bank 1
  (original) and bank 0 (7751 copy).

## Status at handoff

Solid working model of am1 syntax/directives plus all listed peripherals.
Ready to write/review more programs and incorporate further corrections.

# Additions for am1-pdp1-assembler/references/devices.md
#
# Insert this block immediately after the opening paragraph of devices.md
# (after the line ending "...the emulator executes them).").
#
# Target file location (Windows):
#   %AppData%\Claude\local-agent-mode-sessions\skills-plugin\
#   ...\skills\am1-pdp1-assembler\references\devices.md
#
# ─────────────────────────────────────────────────────────────────────────────

---

## Standard PDP-1 typewriter (tyi / tyo) — critical behaviour notes

### tyi — completely asynchronous; does NOT honour the C (in-out wait) flag

`tyi C` does **not** block. `tyi` is always asynchronous regardless of the C or `i` modifier bits.
The emulator executes it immediately: clears IO, copies the typewriter buffer (`pdp->tb`) to
`IO[bits 0-5]`, and clears the type-in status bit (`tbs`). It does **not** clear Program Flag 1,
and it does **not** wait for a key.

Incorrect idiom (will read garbage / stale buffer immediately):
```
    tyi C       // WRONG -- C flag is silently ignored, executes without waiting
```

**Correct idiom** — poll PF1, then read, then clear PF1:
```
    local getkey
getkey,
    szf 1 i     // skip if PF1 is set (key available in buffer)
    jmp getkey  // PF1 clear -- no key yet, keep polling
    tyi         // PF1 set: IO <- typewriter buffer [bits 0-5]; clears tbs
    clf 1       // clear PF1 -- REQUIRED; tyi clears tbs only, not PF1
    endlocal
```

### Program Flag 1 (PF1) and the typewriter

- PF1 is set (`pdp->pf |= 040`) by the emulator when a key is struck.
- PF1 is **NOT** cleared by `tyi` (tyi clears tbs only).
- PF1 is cleared **only** by the `clf 1` instruction (`0760001`).
- `decflg(1) = 040` — the bit mask for PF1 inside `pdp->pf`.
- Without `clf 1` after `tyi`, the next `szf 1 i` sees PF1 still set and skips the poll loop
  immediately, reading the stale buffer character instead of waiting for a new keypress.

### szf / clf / stf — Program Flag instructions

`szf`/`clf`/`stf` are **not** in the same opcode family. `clf`/`stf` are IOT-class (76xxxx);
`szf` is OPR/skip-class (65xxxx). Do not assume they share an encoding prefix.

| Instruction | Encoding | Meaning |
|-------------|----------|---------|
| `szf 1`     | 650001   | Skip if PF1 is **not** set (zero) |
| `szf 1 i`   | (650001 \| i-bit) | Skip if PF1 **is** set (non-zero) — i bit inverts sense |
| `clf 1`     | 760001   | Clear PF1 |
| `stf 1`     | 760011   | Set PF1 |

### tyo — synchronous (`i`) vs asynchronous (`C`) output

`tyo`'s base opcode is **730003** (B5=010000 already included). The `i` and `C` modifiers mean:

| Modifier | Bit  | Effect |
|----------|------|--------|
| `i`      | B5 (010000) | Synchronous — machine waits (ioh=1) until typewriter signals ios=1 |
| `C`      | B6 (004000) | Asynchronous — instruction returns immediately; typewriter signals ios=1 later; use the `ioh` instruction (730000) to sync when the result is needed |

- `tyo i`  = 730003 (B5 only) — **recommended for simple programs**; blocks until char printed.
- `tyo C`  = 734003 (B5+B6)   — async; fires typewriter and continues; sync later with `ioh`.

**`tyo C` requires pdp1.c fix (applied 18-Jun-26).** The original code used `pdp->tcp = nac`;
`nac` is 0 when both B5 and B6 are set, which prevented the typewriter from ever signalling
`ios`, causing a permanent I/O halt. Fixed to `pdp->tcp = !!(MB & (B5 | B6))`.

### am1 reference note: C flag value

The am1 reference documents `C = 040000`. The correct value is **`C = 004000` (B6)**. Evidence:
`tyo C` assembles to `734003`; tyo base = `730003`; `730003 | 004000 = 734003` ✓.

---

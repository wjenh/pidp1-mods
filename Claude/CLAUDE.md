# PiDP-1 Emulator -- Project Notes for Claude

## Hard Constraints
- **`install.sh` must NOT be modified** under any circumstances.
- When `pdp1.h` changes, rebuild BOTH the emulator (`make` in `src/blincolnlabs/pdp1/`) AND all IOT plugins (`make` in `IOTs/Type30Display/` and other IOT subdirs). The user's normal workflow only runs `make` in the pdp1 directory; IOT rebuilds must be done explicitly.

## Design Convention -- Shared Struct Field Ordering
New fields added to shared data structures (`PDP1` in `pdp1.h`, `Panel` in `panel_pidp1.h`) go at the **END** of the struct. This preserves ABI compatibility: IOT plugins compiled against an older `pdp1.h` continue to access existing fields at the correct offsets. Inserting fields anywhere else shifts subsequent field offsets and silently breaks all IOT plugins that weren't rebuilt.

## Architecture

### Display (Type 30 / IOT 7)
- Handler: `IOTs/Type30Display/IOT_7.c`
- When `dpy` fires, reads **`AC(pdp1P)` and `IO(pdp1P)` directly from the PDP1 struct** for X/Y coordinates. It does NOT read from `panel->lights3` or `panel->lights4`.
- `panel->lights*` fields are irrelevant to the display IOT. Extra `updatelights()` calls do not corrupt display coordinates.
- The IOT uses a worker thread (`display.c`) and a command buffer; coordinates are stored via `setDisplayData()` and consumed in `iotPoll()`.

### Panel Driver Interface (`panel1.c`)
- `updatelights(pdp, panel)` -- snapshot only. Writes current register state to `panel->lights0`-`lights9`. No side effects on pdp state, no tally.
- `updatelights_pwm(panel, n)` -- tallies `panel->lights*` into `panel->pwmcount[][]` n times and increments `panel->cyclecount` by n. Call once per completed instruction with n = number of machine cycles the instruction occupied.
- `panel->*` lives in shared memory (`/tmp/pdp1_panel`); the panel driver process reads it independently.

### Machine Cycle Structure (`pdp1.c`)
- One call to `cycle()` = one 5 us machine cycle.
- `pdp->inst_cyc` is incremented at the top of every `cycle()` call and reset to 0 on instruction completion or SBS_BREAK start.
- `TP(n)` macro: fires `updatelights()` once per cycle at a random timing pulse (`timernd`), then sets `timernd = TP_unreachable` so no later TP fires in the same cycle.

### sho/shro Instruction Pipeline (IR = 033)
The shift instruction is **pipelined across two `cycle0()` calls**:

| Phase | What happens |
|-------|-------------|
| Fetch cycle TP7-TP10 | Shifts for bits B17-B13 execute (using current MB = sho instruction word, current IR = 033) |
| Fetch cycle TP10 | `CY0_INST_DONE` fires (TRUE for IR >= 030, df1 = 0) -- but AC/IO are only half-shifted |
| Next cycle TP0-TP3 | Shifts for bits B12-B9 execute (old MB/IR still valid before TP4 overwrites them) |
| Next cycle TP3 | `MB = 0`; AC/IO now hold the fully-shifted result |
| Next cycle TP4-TP5 | Next instruction fetched; IR changes |

**`sho_deferred`** (field in `PDP1` struct, placed at end): At TP10 when `IR_SHRO` and `CY0_INST_DONE`, save `inst_cyc` to `sho_deferred` and reset `inst_cyc = 0` instead of tallying. After `MB = 0` at TP3 of the next cycle, if `sho_deferred != 0`: call `updatelights()`, then `updatelights_pwm(panel, sho_deferred)`, clear `sho_deferred`. Also clear `sho_deferred` on SBS_BREAK. This ensures the panel PWM tally sees the fully-shifted AC/IO state, not the intermediate half-shifted state.

## Key Files
| File | Role |
|------|------|
| `src/blincolnlabs/pdp1/pdp1.h` | PDP1 struct definition; shared by emulator and all IOT plugins |
| `src/blincolnlabs/pdp1/pdp1.c` | Core emulator: `cycle()`, `cycle0()`, `defer()`, `cycle1()`, `brkcycle()` |
| `src/blincolnlabs/pdp1/panel1.c` | `updatelights()` / `updatelights_pwm()` -- panel register snapshot and PWM tally |
| `src/blincolnlabs/pdp1/main.c` | Main loop; halt-state and DMA-steal must each call `updatelights()` + `updatelights_pwm(panel, 1)` |
| `src/blincolnlabs/pdp1/display.c` | Type 30 display worker thread; `setDisplayData()` / `getDisplayData()` |
| `IOTs/iotHandler.h` | IOT plugin interface; includes `pdp1.h` -- ABI-sensitive |
| `IOTs/Type30Display/IOT_7.c` | dpy IOT handler |
| `IOTs/Type30Display/Makefile` | Builds `IOT_7.so`; must be run after any `pdp1.h` change |
# SYSTEM PROMPT ENVIRONMENT: ISO C (MODERN GCC) WITH VERBOSE DEFENSIVE STANDARDS

## 1. TARGET ENVIRONMENT & STABILITY PRIORITIES
*   **Language & Toolchain:** ISO C matching modern GCC default behaviors. Procedural/structural hygiene.
*   **Architecture & OS:** Cross-platform Linux (Intel/AMD x86_64 and high-performance ARM, Pi 4 and above).
*   **Optimization Vector:** Prioritize correctness, strict predictability, and structural hygiene over micro-optimizations.

## 2. LIFECYCLE & TRACEABILITY
*   **Iterative Design:** Step-by-step logic expansion with clear validation checkpoints.
*   **Design Mapping:** All code must explicitly map to conceptual modules, state machines, or design requirements.

## 3. EXTREME COMMENTARY MANDATE
*   **No "Self-Documenting" Assumptions:** Comprehensive textual explanations are mandatory.
*   **File/Module Headers:** Must include purpose, architectural scope, dependencies, and execution model.
*   **Function Contracts:** Block comments defining preconditions, postconditions, arguments, return behaviors, and edge-case handling.
*   **Inline Logic:** Granular commentary detailing the "why" and "how" of branches, state changes, math steps, and pointer manipulation.

## 4. DEFENSIVE SYNTAX & FORMATTING BLUEPRINT
*   Indent with 4 spaces, no tabs in text
*   **Grouping Operators:** Mandatory parenthetical containment `( )` for arithmetic, logical, and bitwise operations to completely isolate precedence bugs.
*   **Control Flow Blocks:** Explicit braces `{ }` for all control constructs (`if`, `while`, `for`, `do`), with braces positioned on separate lines from the body.
*   **Function Calls in Conditionals:** Permitted for common variable assignments where the result can be tested in an obvious, explicit manner (e.g., `if( !(fP = getFile(...)) )`).
*   **Spaces:** No space permitted between a directive/control keyword and its opening parenthesis (e.g., `if(`, `while(`, `switch(`).
*   **Variable Scope Declarations:** 
    *   Globals: Declared strictly at the beginning of the file.
    *   Locals: Declared strictly at the beginning of the function body.
    *   Instantiation: Explicit separation of declaration and initialization in most cases.
*   **Naming Typography:** Camel-hump naming convention.
*   **Pointer Conventions:** C-style pointer declarations (`char *ptrP`) featuring trailing Systems Hungarian Notation suffix matching the pointer type (`P` or `ptr`).

## 5. FILE LAYOUT CONFIGURATION (LOOSE SCOPING ORDER)
1.  Comprehensive File Block Header
2.  Include Files (`#include`)
3.  Preprocessor Defines (`#define`)
4.  Global Variables (Grouped cleanly by data type)
5.  Function Declarations / Definitions

# Reading the workstation code

Start with `workstation.am1`. It lays out the four memory fields and runs the main loop: acquire 256 samples, compute FFT/power, publish a new scene, then hold or advance according to the sense switches. Source is AM1 assembly with C-style preprocessing; decimal constants use `0d`, and unmarked numeric constants are octal.

## Files and responsibilities

| File | Purpose |
|---|---|
| `workstation.am1` | Memory layout, startup, hold/run/next controls and field-to-field calls |
| `kernel.ac` | Bit-reversed input loading, radix-2 FFT and full-product power |
| `state.ac` | Numerical scratch words; the kernel is non-reentrant |
| `frontend.ac` | Directory/track validation, frame acquisition and construction of display lists |
| `ui-state.ac` | Acquisition and scene-building scratch/state |
| `tape-loader.ac`, `tape.py` | Read-only guest loading, recording validation, tape preparation and configuration |
| `display.ac` | Channel-0 sequence-break handler, buffer handoff and refresh timing |
| `generate_tables.py`, `generated/` | Fixed twiddles, bit-reversal permutation and static display commands; no host FFT |
| `drum.py` | Host-side image validation and guarded selection/backup/restore |
| `live.py` | AD1 loading with core snapshots and read-only status reporting |
| `import_wav.py` | Optional host decoding/resampling/quantization into the documented sample format |

## Arithmetic contract

Samples are signed 18-bit one's-complement fractions with divisor 131072. Both zero encodings are accepted; the importer writes canonical positive zero. One input guard shift and eight stage shifts produce a stored FFT gain of X/512. Right shifts truncate toward zero, including negative odd values. General twiddle products use the Type 10 MUL high result; exact unity and -j rotations bypass approximate coefficients.

Power retains the complete product: `S = R*R + I*I`, stored in two unsigned 17-bit limbs, `phi*131072 + plo`. One-sided calibrated power is `4*S/2^34` at DC/Nyquist and `8*S/2^34` at interior bins. X4/X64 affects display coordinates only. Consult `signal-analysis-workstation.md` for the complete derivation and evaluation order; the running source and current guide supersede earlier proposals in that history.

## Memory and calling conventions

- Field 0: interrupt vector at 0–3, status words at 0100–0102, numerical state, interrupt scratch/handler, main code, FFT arrays, twiddles and input.
- Field 1: acquisition/scene state, validated directory, retained waveform, power arrays, frontend code and static display templates.
- Field 2: two 1984-word display lists.
- Field 4: tape configuration, loader, header and Bill's filesystem state; fields 5–15: preloaded samples (11 seconds maximum).
- Field 3: a full 4096-word sample-track cache. It is filled with 128-word read-only DMA requests, then validated as a whole before analysis. After each request, the frontend allows the display to finish a pass before issuing the next; this bounds drum-channel interference at the cost of slower track loading.

A cross-field entry saves the entire JSP return word, uses LEM for local work, then EEM and an indirect jump to return. A local helper saves only the local return address with DAP. Saving a complete field-1 return word for a local indirect jump would make bit 010000 act as another defer bit. AC/IO are scratch except where explicitly preserved; ordinary numerical/frontend helpers are non-reentrant.

The interrupt handler has separate scratch and never calls those helpers. The sequence-break hardware saves AC, packed overflow/extend/PC and IO at 0–2. `display_isr` restores IO/AC and returns through `jmp i 1`, which restores overflow and addressing state too. Startup owns channel 0 and disables other channels in SBS16 mode. Drum completion can share channel 0: the handler returns when no display halt is present. The read loop must not use CBS to discard a drum completion, because that could also discard a display break.

## Display ownership and timing

`active` is immutable while the Type 340 scans it. `build` points to the other list. `EMIT` bounds-checks every generated word. After the list is complete, the CPU posts it to `display_next` and waits for acknowledgement. At HALT + interrupt, the handler adopts that list and clears the mailbox before returning to the CPU. Only then may the CPU reuse the previous list. This applies to both new FFT frames and gain-only redraws.

The BBN millisecond counter times successive display completions, accounting for its 60000 ms wrap. Status exposes last/min/max and a count over 25 ms. This is separate from the much slower rate of analyzing new 62.5 ms recordings. A 1 ms run-clock measurement does not establish a hard wall-clock latency guarantee.

The Type 340's vector BRM count grows in powers of two, from 3 to 7 bits. A unit-spaced slope with magnitude 64 takes 127 pulses, nearly twice a magnitude-63 slope. This explains why a list's word count alone does not predict scan time. The optimized frontend keeps short waveform segments at unit spacing. For 16–31-pixel rises it draws the vertical bulk with spacing 4; for larger rises it uses spacing 8. A short unit-spaced tail reaches the exact next sample endpoint. Every sample remains represented; intermediate dots on steep slopes are coarser. Faint horizontal guides use spacing 4. Spectrum bars use spacing 4 for heights of at least 16 pixels and unit spacing for the remainder, retaining their exact calibrated/clipped heights. A 256-pixel bar is split into 63+1 coarse steps to avoid the BRM boundary at 64. Its initial positioning header also selects spacing, avoiding a redundant parameter word per bar and keeping list size bounded.

## Errors, checks and future changes

The CPU halts on invalid headers, DMA failure/timeouts, checksum mismatches, display edges or list overflow. The fault codes are listed in README. The drum selector independently refuses a running CPU, drains the stopped device handle and saves verified backups before publishing another image. Core snapshots are a separate loader precaution; neither helper automatically restores an interrupted whole-machine session.

The development suite checks arithmetic against an independent integer model, buffer immutability, all controls, sample/checksum errors, clock wrap, SBS modes and vector endpoints/bounds. `display_cost.py` in the development tests estimates nominal scan cost from the installed emulator's BRM, position and character models; live timing is checked separately. The distribution's `make check` checks package/sample integrity only.

The v4 tape backend feeds the same 256-sample contract into acquisition without changing FFT arithmetic. Microtape loading now uses Bill's filesystem library during startup, then the same acquisition interface reads RAM. See `MICROTAPE.md` for its complete contract. PENMENU/PENKBD are candidates for later interactive controls; neither is silently enabled by this preview.

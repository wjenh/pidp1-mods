# Validation of the 10 November 2026 preview v3

The source and preassembled program in this package match the version running on Phillip's PiDP-1 with Type 23 acquisition and Type 340 display. Hardware/device interfaces have been tested on that installation; Bill's installation remains a compatibility test.

Completed development checks:

- 154 numerical FFT frames against an independent integer model, covering all tone bins, impulses, both zero encodings, random full-scale data and small signals; 63 crafted full-product power carry cases.
- Directory/sample checksum and DMA error paths; an 18-frame/two-track sequence; active-display-list ownership; production hold/run/single-step controls; steep full-scale waveform vector splitting.
- 19 guarded drum-switch tests, including stopped-device handle closure, backup and recovery behavior.
- 11 WAV conversion tests, including high-precision PCM, explicit gain/clipping, channel selection, duration limits and anti-alias suppression exceeding 60 dB for the checked out-of-band tone.
- All 144 imported Apollo frames plus two wraparound frames against the independent power model, including the partial last track, at X64 gain. No FFT overflow or display-list fault occurred.
- X4/X64 equality of stored FFT/power, quiet-tone visibility, endpoint weighting, loud-tone clipping, twelve full-scale noise cases, and gain changes in both directions while holding. The held frame counter stayed unchanged. Largest checked gain-case list was 1415 of 1984 words.

These CPU integration checks use the installed emulator CPU object with deterministic device adapters. They verify instruction execution, data and device-call sequencing; they do not emulate all real device timing. Live checks on the Pi separately confirmed acquisition, continuous refresh, the displayed controls and spectra, and zero fault/overflow counters. The user confirmed the gain change looked correct.

`make check` in this package checks file hashes and recording format/checksums only. It does not rerun the development CPU tests or certify another machine's device configuration. The complete development suite remains in the Mac development workspace.

The documents `signal-analysis-workstation.md` and `target-preflight.md` record the design and integration history, including earlier proposals. This README, TRY-IT guide and running source describe the shipped behavior.

## Display update prompted by Bill's review

Small labels were enlarged from scale 0 to scale 1, and all label intensities reduced from 7 to 5. The CPS label was moved right to avoid overlap with the enlarged 2048 label. Live screenshots confirmed the layout.

The polling restart was replaced with a bank-0 sequence-break handler following the approach in `FunStuff/Sketchpad/spbreak.ac`. A posted inactive list is adopted only at a display HALT; the previous list is not reused until the handler acknowledges it. Clock-based status records min/max/last completion-to-completion intervals (the first is timed from initial start) and counts passes exceeding 25 ms. The handler restores AC, IO, overflow and addressing state.

Additional assembled CPU tests cover both single-channel and SBS16 operation, the 60000 ms clock wrap, and a deliberately slow 35 ms display. Normal simulated completions measured 5–6 ms with no budget misses; slow simulated completions measured 35–36 ms and every pass was correctly counted over budget. These are adapter tests, not real display-speed claims. Existing frontend/gain checks and all 146 Apollo power comparisons pass with the interrupt handler active.

An initial eight-second live speech measurement on Phillip's Pi gave 53.74 refreshes/second; the clock recorded 14–47 ms intervals, with 27 of 431 completed passes exceeding 25 ms. Refresh improved materially over the earlier approximately 25 passes/second observation, but the every-pass 25 ms goal is not yet met. No FFT overflows or faults were observed, and the active drum hash was unchanged. The clock is a 1 ms run-time counter, so host scheduling can also affect perceived wall-clock refresh.

Microtape sample loading is a planned extension, not part of v2. Existing Type 550 definitions, Sketchpad's tape code, PENMENU and PENKBD were reviewed. Bill's forthcoming filesystem library should provide the storage interface; the current signed-sample and FFT contract can remain unchanged. No filesystem format has been invented for this preview.

## V3: identify and shorten the long scans

The v2 49 ms tail was consistent with real drawing cost, not simply late interrupt handling: a decoder using the installed Type 340/342 timing model predicted 46.1215 ms for the slowest generated Apollo scene. Its waveform category accounted for 28.75 ms, including the original long horizontal waveform guide. Vector cost follows the shortened 3–7-bit BRM counter; a 64-step vector takes 127 pulses rather than the 63 needed for a 63-step vector. Word count alone misses this cost.

V3 preserves all sample endpoints while using dot spacing 4 for 16–31-pixel rises, spacing 8 for larger rises, and a unit-spaced remainder to reach the exact next sample. Short slopes remain at unit spacing. Bars use spacing 4 with an exact remainder; a 256-pixel bar uses 63+1 coarse steps to avoid a BRM boundary. Horizontal guides use spacing 4 and are dotted. This changes intermediate beam-dot density, not source samples, FFT arithmetic, power, gain or endpoint coordinates.

Across all 144 speech frames plus two wrap frames, nominal worst scan time fell from 46.1215 to 23.1465 ms (50% lower), and mean nominal time from 20.1711 to 14.8899 ms (26% lower). No nominal scene exceeded 25 ms, compared with 36 before. This estimate excludes host scheduling, channel contention and interrupt latency. It is specific to the checked recording.

The independent scene check verified all 37,376 sample endpoints, every nonzero weighted/clipped power bar, and all coordinates. Largest scene was 1414 words of 1984. The full fixed-point power oracle still matched all 146 frames. Frontend/control/error, gain, buffer ownership, SBS modes and clock-wrap checks also passed. The complete development tests remain on the Mac and in the temporary development area, rather than cluttering the installed program directory. Source comments and `CODE-TOUR.md` explain the arithmetic, field-local returns, buffer ownership and optimization.

The first live vector-only test held scan intervals to 24 ms through the formerly slow waveform frames. A single 40 ms pass at the first track boundary revealed a second cause: the full-track drum request uses a higher-priority memory channel for approximately 34.8 ms. V3 therefore fills the same track cache in 128-word requests (~1.1 ms of device transfer each), allowing a display boundary between requests. The whole track is still checksummed before analysis; the tradeoff is slower track loading. The old read-loop CBS was also removed because clearing all pending breaks while the display ISR is enabled could lose a display completion. Simulated DMA now raises shared-channel completion breaks, and rejects requests larger than the intended 128-word limit.

A 75-second live test of the scaled drawing plus bounded DMA covered two track transitions and recorded 4511 completed passes, 11–25 ms intervals, zero passes over 25 ms, zero faults and zero FFT overflows. The selected drum hash was unchanged. This is an observed run, not a real-time guarantee for all host loads or future recordings. A final equivalent stream compaction selects each bar's spacing in its positioning header, removing one redundant word per nonzero bar; the endpoint/power tests are repeated for that compact stream.

## V4: read-only Microtape preload

The station now uses Bill's FILESYSTEM/MICROTAPE 2.1 library at installed revision `a81ef4b99786ff04624ae8529c2bc6b826f767c1`. Startup reads and validates a named file into fields 5–15, then the existing FFT/power engine and display run from RAM. The drum path remains available. A field-1 loading/error scene loops without completion interrupts while tape calls have sequence breaks disabled.

The isolated tape runner links the installed PDP-1 CPU object with the real Type 550 control, transport and shim source, enabling the PDP-1D instructions used by the filesystem. Its drive-list configuration is isolated; test images are mounted explicitly locked with creation disabled. The runner rejects guest MMT, write and erase IOTs. This differs from the simpler completion adapters used for the original drum tests.

Checks passed:

- All 36864 Apollo sample words loaded into the nine RAM fields matched the original drum samples, including the 100-word zero tail.
- Every power bin in all 144 frames plus two wrap frames matched the independent integer oracle; no drum reads occurred in tape mode.
- Tape images remained byte-identical after the tests.
- Missing image/file, wrong contract, oversized RAM length, bad header checksum, bad payload checksum, short header and short payload were rejected with no FFT publication.
- Six host/container/drive-selection checks passed, including preservation of other assignments, assignment backup, rejection of an occupied drive and the eleven-second limit.
- The existing drum frontend/hold/step/gain, clock/SBS and eleven WAV importer checks passed with the new build.

On Phillip's Pi the first real Apollo load and first FFT completed in 14.65 wall-clock seconds. Source reported tape, loader stage 9, status 0, no faults or FFT overflows. The next 30 seconds of continuous analysis, including a logical track transition, measured 12–24 ms display intervals with zero passes over 25 ms. The active drum and tape hashes were unchanged; pre-existing drive assignments were preserved. The new drive-8 assignment is explicitly locked and its previous assignment file is saved in snapshots. These timing observations are not a hard guarantee for every host load.

The guest does not use the image-mount extension: source inspection showed it can create a missing image. The host validates an existing image and adds a locked drive assignment before execution; the guest only mounts the filesystem on that drive. The completed program uses 2976 words of field 4, leaving 1120 words free there; full RAM mode requires the 64K setup. The compact recording format and operational limits are documented in `MICROTAPE.md`.

A repeated live load exposed a device-reported missed word (FS_EBADBLOCK), so the loader now retries that read at most five times, restoring the descriptor position with fsSeek before retrying. An injected one-word timing loss recovered with identical samples; persistent injected failures stopped after five attempts without publishing a frame. The autonomous loading/error scenes were also paced with unlit positioning words to avoid excessive text redraw while tape polling is active. These changes preserve all subsequent sample and power checks.

Because the live host occasionally missed a few words in a long transfer, the final loader uses independently retryable 1024-word reads, at most five attempts per read, rather than restarting the entire recording. Injected missed-word recovery verifies every resulting RAM word, and a warm-tape test starts the head beyond the recording and verifies reverse search and loading. The final full Apollo test again matched all samples and 146 spectra.

The final chunked live load completed in 39.74 seconds with no retry needed in that run. All 36864 words were read back from the live machine's RAM and matched the original Apollo payload exactly. Subsequent 30-second continuous analysis measured 12–26 ms with one interval over 25 ms among 1840 completed passes; no faults or FFT overflows occurred. The raw drum hash was unchanged. This replaces the early single-read prototype as the release behavior.

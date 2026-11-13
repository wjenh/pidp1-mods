# Installed PiDP-1 preflight and first implementation

Checked over SSH on 10 November 2026 as `ppporch@pdp1`.

## Installed target

| Item | Observed result |
|---|---|
| Repository | `/opt/pidp1-mods` |
| Revision | `53871ec0450f24f97ac178e016350c85fe1fce09` |
| Last commit | 8 November 2026, update to match current AM1 output |
| Tracked working-tree changes at preflight | None; existing untracked games, builds, saves and drum data were left in place |
| Assembler | AM1 v5.7, 6-Nov-2026, available through `/usr/local/bin/am1` |
| Legacy assembler | `/usr/local/bin/macro1` points through `bin/macro1_1` to the source-tree executable |
| Debugger | AD1 v2.9; installed AM1-aware fastload also available |
| Core | CPU has a fixed 65536-word core array, enough for the planned operating fields |
| Configuration | `muldiv=on`, `sbs16=off`; PDP-1D extensions are also enabled in the normal configuration |
| Type 23 | IOT_61.so, IOT_62.so, IOT_63.so installed |
| Display | Type 340 selected by user; IOT_15.so, IOT_16.so, IOT_17.so installed. Original Type 30 module also remains installed |
| Normal emulator | Running from `/opt/pidp1-mods`; fixed `/tmp/pdp1.lock` prevents a second normal instance |
| New implementation | `/opt/pidp1-mods/FunStuff/SignalAnalysis` |

The configuration-file observations are not a readback of live CPU option registers. The isolated tests explicitly enabled Type 10 MUL and disabled all PDP-1D extensions. No debugger connection, load, start, halt, or restart was issued to the user's normal emulator.

## Important correction to the design

The installed drum module defines `DRUMFILE` as `/opt/pidp1-mods/pdp23drum` in its source. There is no existing drum-image selection setting. The normal configuration filename is also hard-coded by main.c. A separate working directory does not isolate either file, and the single-instance lock prevents running a normal second emulator.

The existing pdp1central Drums tab supplies the required controlled switch: `.drm` images in `/opt/pidp1-mods/Drums` are library copies, and Pick copies the selected image onto the fixed active filename after backing up the current image to `pdp23drum.bak`. The installed `putDrum` and `pickDrum` implementations confirm this. The drum plugin detects the replacement and reloads it at the next transfer. No emulator or configuration change is required.

Use `Drums/SignalAnalysis.drm` for the workstation. The new `drum.py` helper and `make drum-image`, `drum-status`, `drum-pick`, and `drum-restore` targets implement this workflow and add verified permanent backups, checksums, a recovery journal, and stopped-engine/write-close checks. Reach an idle point with no transfer in progress, halt the PDP-1, use Save drum to preserve the previous image under a durable library name, then Pick the workstation image and load/start its program. Pick does not automatically halt the machine or wait for pending writes. The automatic `.bak` is replaced by a later Pick, so it is not a durable named backup. Restore the previous image through Pick while idle and halted; reload its program rather than resuming a program expecting the other drum.

This resolves image selection using the existing library. The calibration image has now been created and selected, with the previous image preserved under `Drums/Before-SignalAnalysis-20261110T170838.749964Z-3dbd7a15.drm` and `pdp23drum.bak`. Previous SHA-256: `2528c2f0d8a085a6a2b6808118799c6b1eed097ebff13d5603c545c312cde027`; selected SHA-256: `c19afccc6af715663729459a1b9554e404a7fd6d6fb087ab826c19cbfbccfe93`. All 15 switching tests passed on the Pi, including real `/proc` descriptor detection on a disposable file. The user turned on power during the work; the engine remained halted during the switch. No program was loaded/started during the switching milestone. The subsequent workstation milestone saved core snapshots, loaded the new program and started it; normal runtime configuration and emulator source were not changed.

## CPU object used for verification

The independent test runner links the target's existing CPU object and calls its normal cycle dispatcher. Device and debugger hooks are inert; RPB is implemented only as a deterministic word stream for the AM1 bootstrap check. This is not a second complete emulator instance and does not acquire the normal emulator's lock or shared panel.

Preflight SHA-256 fingerprints:

```text
CPU source: aa73d6d9816cd6ff70e98c361d203d456d9631612ff259598128fcb3b51322aa
CPU object: e3abb803b97b96c6164dc3e15ee3022908611b15ba7d7d14de867d9421a6c014
Type 30:    607ce0d11fc20e8f9c6cfd1d1ada0be553af2179c16c80d6a623afde30ec825f
Type 23/61: b8e9d564bded0031c3dd14167d3b438daa7d40106f5cc0434be16ded561ab498
```

## Implemented and verified

- Original-instruction-set arithmetic and extended-memory diagnostic.
- In-place 256-point DIT FFT with separate arrays, input bit reversal, initial guard shift and eight fixed stage shifts.
- Exact unity and -j rotations; inward-truncated stored coefficients otherwise.
- Exact R^2+I^2 for all 129 bins, retaining full MUL products and low-limb carry.
- AM1 tape/listing agreement and execution of its actual extended-memory bootstrap using the installed CPU object.
- 154 numerical frames, including every positive-frequency tone bin; every decoded output matches the independent integer oracle, with no observed FFT overflow.
- 63 deliberately exercised power low-limb carry cases.
- DFT spot checks: maximum checked complex error 7.553618 stored-result counts.
- Example off-bin FFT plus power: 149934 memory cycles, 767596500 simulated nanoseconds (about 0.768 seconds). Includes preparation/control overhead; excludes display, drum, and wall-clock pacing. AM1 bootstrapping of the same case brings it to about 0.882 seconds.

## First live workstation milestone

The first interactive workstation is implemented and running on the Pi. Production source is `workstation.am1`, with `frontend.ac` for track acquisition and Type 340 generation. Sense Switch 1 selects hold/continuous; a Sense Switch 2 on/off pulse advances one frame in hold mode. The startup scene is track 1/frame 0 of the selected calibration image, with correct 512 and 1024 CPS peaks. The display viewer still has a Type 30 title; rendering commands are issued by the Type 340 controller.

Implementation refinement: use field 3 as a complete read-only track cache, bringing the operating allocation to four fields. It is checksummed after each DMA cache miss. Field 2 has two 1984-word display lists. A full-cache build uses AD1 and cannot retain Extend DDT's bank-end scratch layout unchanged.

Integration verification passed calibration acquisition, exact FFT/power comparison, an 18-frame/two-track sequence, active-list immutability, production hold/run/next controls, worst-case steep-waveform vector splitting, and fail-closed directory/sample-checksum and DMA-status cases. The original 154 numerical cases and 15 drum-switch tests still pass. The calibration list is 649 words; the steep-transition list is 1412 words.

Live inspection confirmed power on, CPU running, Sense Switches 00, track 1/frame 0 held, done=1, no FFT overflows, fault=0, and strongest bins 32/64 (512/1024 CPS). Measured hold-mode refresh was about 25 passes per wall-clock second. A screenshot verified the connected waveform, labels, frame indicator and two spectrum bars. Startup in the isolated adapter test took about 2.05 simulated seconds including full directory/sample validation; that adapter does not model real display/DMA timing.

The pre-workstation core and register snapshot are preserved under `snapshots/20261110T173451.266629Z.mem` and the matching JSON. Later reloads keep additional snapshots. The selected drum remains unchanged, with its original named backup retained. Remaining enhancements are previous-frame/track selection UI, adjustable gains, windowing, light pen and a WAV importer; none is required to operate this initial calibration workstation.

## Changing calibration recording

`Drums/SignalAnalysis-Changing.drm` is now selected (signal ID 2, SHA-256 `e5397d59128f76e190acbb41e2f0804cf395365ddf72dd92af1a3af8d6aebffa`). Frames 00-03 contain a half-scale 256 CPS tone, 04-07 a half-scale 512 CPS tone, 08-11 that tone plus a quarter-scale 1024 CPS tone, and 12-15 the quarter-scale 1024 CPS tone alone. Every frame and wraparound were verified against the independent fixed-point oracle using the installed CPU object over 18 published frames. Live observation saw all four working spectra with Switch 1 on and no faults or overflows. Working power can lead the published display briefly while a new list is built.

The original repeating calibration remains in `Drums/SignalAnalysis.drm` and was saved again as `Drums/Before-SignalAnalysis-20261110T175651.699973Z-ac3b9dd2.drm`. The original pre-workstation drum remains in the earlier named backup. The rolling `.bak` now holds the repeating calibration. A new core snapshot was saved before restarting the analyzer, preserving the physical sense-switch settings.

The installed main loop can reopen a plugin handle on the last running cycle after an initial Stop notification. A second Stop while already halted closed that handle without CPU execution; the switching helper now performs that preparation automatically before checking that write-back has closed the file. It still refuses a running CPU and waits for closed descriptors. All 19 switching tests pass, including this preparation behavior. No emulator source or configuration was changed.

## WAV import and Apollo 11 recording

WAV import is implemented with the Pi's existing FFmpeg/ffprobe tools and Python standard library. It supports uncompressed PCM and IEEE-float WAVs, explicit channel selection/averaging and gain, anti-alias conversion to 4096 Hz, canonical 18-bit quantization, up to 31 sample tracks and validated partial-track padding. Output/provenance files are exclusive and the importer refuses the active drum path. Default gain is 1; no automatic normalization, host FFT, DC subtraction or windowing occurs.

The selected real-data image is now `Drums/Apollo11.drm`, using NASA/JPL-Caltech's Earth-version Armstrong recording from https://science.nasa.gov/resource/sounds-of-mars-one-small-step/ . The unchanged source WAV is retained under the workstation's audio directory. Source SHA-256 is `9006c290b776d9163158ff3013710ce78af5634a1091925004b4d6f9de044990`. The source is 48000-Hz stereo PCM16 and 8.975625 seconds long. Import uses equal-weight mono mixing and explicit gain 2, with peak about 0.838 and no clipped samples. There are 36764 valid 4096-Hz samples on nine tracks, 144 frame slots and 100 padding samples. Output SHA-256 is `3b23f2e492969263b8abd93c06c7cdc18f2d70b24d1bc5f5618baeefd16a3b17`; the source URL, credit, settings and hashes are in the adjacent `.drm.json`.

Eleven converter tests passed on the Pi, including preservation of 24-bit source precision down to the 18-bit LSB and rejection of non-finite floats, channel mixing, gain/clipping, trims and full capacity. The anti-alias test passes 512 CPS and rejects a 3000-CPS source by more than 60 dB. All 144 imported frames plus two wrap frames matched the independent integer oracle using the installed CPU object and adapters. All nine track checksums and the partial tail were exercised; the maximum plotted bar height was 60 units at X4, with 136 nonzero frames out of 146 checked.

The earlier changing calibration was saved under a new named backup before selecting Apollo11. A core snapshot was saved before restarting. The production PDP-1 program did not change. The user enabled Sense Switch 1 for sequential speech analysis. Normal runtime configuration and emulator source remain unchanged.

## Selectable spectrum gain

Sense Switch 3 now selects X64 (on) or X4 (off) calibrated power display gain. The high-power limb is used directly for X64, shifted four places for X4, and halved at DC/Nyquist; heights are capped at 256. The gain label and bars latch the same setting. A gain change in the production idle loop rebuilds the held frame without advancing the frame count or recomputing the FFT. Active-list ownership and asynchronous refresh are preserved.

Assembled CPU checks passed quiet-tone visibility (zero pixels at X4, one at X64), identical FFT/power at either gain, loud-tone clipping, DC/Nyquist weighting, twelve full-scale noise cases, steep waveform bounds, and held-frame switching in both directions. Largest list in these gain cases was 1415 words of 1984. All 144 Apollo speech frames plus two wrap frames matched the independent power oracle with X64 selected, with no overflow or list fault. Of 146 checked frames, 142 had nonzero bars at X64 versus 136 at X4. The selected recording and all stored analysis values are unchanged.

## Bill's display review: interrupt refresh and larger labels

Bill's review identified small bright labels and a missing measured 25 ms refresh budget. The old cooperative polling observation was about 25 passes/second, not 25 ms/pass. The v2 frontend uses scale-1 labels at intensity 5, with the CPS label moved to preserve spacing. A bank-0 channel-0 sequence-break handler, following Sketchpad's display completion approach, restarts at HALT + interrupt and acknowledges posted inactive lists at the stopped-frame boundary. It restores AC, IO, overflow and extend state. A BBN clock counter records last/min/max intervals and passes over 25 ms with minute-wrap handling. Startup owns channel 0 in single-channel and SBS16 modes and disables other channels.

Both SBS modes, clock wrap and deliberately slow-frame accounting passed adapter tests. Frontend and gain checks, 154 numerical frames, 63 power-carry cases, 19 drum-switch tests, 11 importer tests, and all 146 Apollo power comparisons pass. The Pi's initial eight-second live sample averaged 53.74 refreshes/second with 14–47 ms clock intervals and 27/431 passes over 25 ms. Thus refresh improved, but the every-pass 25 ms target remains unmet. This is measured run-clock timing, not a wall-clock scheduling guarantee. Source/program matches are retained in the v2 share package; the recording hash is unchanged.

Microtape sample loading should use Bill's forthcoming filesystem library. Type 550 definitions and Sketchpad's tape examples are available; PENMENU and PENKBD provide reusable future controls. These are design directions, not features added by this display update.

## V3: scan-cost diagnosis and bounded drum requests

A decoder of actual generated lists using the installed Type 340/342 fetch, point-position, character and BRM timing model predicted a 46.1215 ms worst Apollo scene, closely explaining the observed ~49 ms refreshes. Steep waveform lines dominated. V3 draws vertical bulk at spacing 4 for 16–31 pixel rises and spacing 8 for larger rises, followed by a unit-spaced tail to the exact next sample endpoint. Short slopes stay at unit spacing. Bars use spacing 4 plus an exact remainder; 256 pixels is split 63+1 coarse steps to avoid a BRM counter boundary. Faint horizontal guides use dotted spacing 4.

All 146 actual scenes (144 Apollo frames plus wrap) passed independent endpoint/bar/bounds checks: 37,376 sample endpoints retained exactly; largest scene 1414/1984 words; all full-product spectra unchanged. Worst modeled scan fell to 23.1465 ms and mean to 14.8899 ms, with zero nominal scenes above 25 ms. The model excludes scheduling, contention and interrupt latency, and is specific to the checked recording.

The vector-only live check stayed within 24 ms through the formerly slow frames, then recorded one 40 ms pass at a track transition. The complete-track Type 23 request uses a higher-priority memory channel over ~34.8 ms. The final v3 loader instead fills the same cache in 32 read-only 128-word requests and allows a display boundary between requests. Whole-track checksum validation is unchanged. The cache fills more slowly, bounding interference at the cost of load latency. The obsolete read-loop CBS was removed: with interrupts enabled, it could clear a pending display completion too. The adapter now raises drum completion breaks and rejects oversized requests; control, error, SBS/clock and all nine-track power checks pass.

`deployment/CODE-TOUR.md` explains source navigation, arithmetic, calling conventions, memory, asynchronous ownership, timing and the beam-spacing tradeoff. Additional comments document the renderer and cache-fill changes.

## V4: read-only Microtape source

Bill's FILESYSTEM/MICROTAPE 2.1 at a81ef4b99786ff04624ae8529c2bc6b826f767c1 is now integrated as a startup-only loader. The host preflights an existing tape recording, backs up tape assignments and selects drive 8 with ,locked; the guest deliberately issues no MMT, format or write calls. It reads a compact recording-v2 header and payload into fields 5–15, validates all counts/checksums/padding, then uses the same FFT input and power pipeline from RAM. Existing raw-drum operation remains available. The configuration and loading/source labels are generated consistently; naming another file requires rebuilding.

Filesystem calls run with interrupts off. The loading/error scenes loop natively in field 1 and are paced with unlit positioning words. Normal display publication resets DLA and enables the existing ISR only after the recording is validated. Field 4 ends at 045640 octal (2976 words occupied), leaving 1120 words there; RAM mode allows eleven logical seconds on the 64K setup.

Long native reads occasionally reported missed words, so the release reads 1024 words at a time, with at most five attempts for FS_EBADBLOCK and fsSeek back to the original descriptor position before retrying. Invalid application data is never analyzed. Tests using the real CPU/Type 550 source verified a forced missed-word recovery, stopping after five persistent failures, loading from an already-used tape position, eight malformed/missing-input paths, all 36864 Apollo samples and 146 exact oracle power spectra with zero drum reads. The host data/drive tests and existing drum/control/WAV tests also pass.

The final live load took 39.74 seconds; all 36864 live RAM words matched the source. The next 30 seconds ran without faults/overflows, with 12–26 ms refresh intervals and one budget miss among 1840 passes. Prior tape assignments and active raw drum data are preserved. The drive-8 sample image is read-only and contains APOLLO11, TWOTONE and CHANGING. Operational details and the frozen recording-v2 layout are in deployment/MICROTAPE.md; the older integration review remains historical design work.

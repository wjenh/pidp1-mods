# Bill: setup and first test

This preview is intended for an existing PiDP-1 installation with the modified emulator/device interfaces described below. This is v4 with read-only named Microtape loading. It does not install or reconfigure the emulator. Phillip has tested it on his Pi; a second-machine compatibility test is the next step.

## Required setup

Tested base: `pidp1-mods` revision `a81ef4b99786ff04624ae8529c2bc6b826f767c1`, with AM1 5.7 and AD1 2.9. A compatible newer revision may work, but has not been checked here.

- Linux Pi, Python 3, and read/write permission for the PiDP-1 installation and access to its emulator process information.
- Extended memory: use the full 64K-word setup for the tape loader. Fields 5–15 hold the recording.
- Hardware multiply/divide enabled (Type 10 arithmetic). The numerical kernel uses original instructions, but the filesystem requires `core1D` or `all1D` enabled.
- Type 550 Microtape support and `bin/mtfs` from the inspected filesystem revision, if using tape mode. Drive 8 must be free or already assigned to the supplied tape.
- Type 23 drum enabled at IOTs 061–063, with the installation's 32-track, 4096-word, little-endian 32-bit-slot image format.
- BBN timesharing clock enabled at IOT 032 for its 1 ms counter. Display refresh uses sequence-break channel 0 (single-channel or SBS16); the program installs its own handler and disables other channels at startup.
- Type 340 display enabled at IOTs 015–017, with Type 342 character support and the existing display viewer open.
- Local emulator command port 1040 and compatible AD1 debugger port 1044. `bin/fastload` must support AM1 extended-memory RIM images and the `-y` load/start option.
- Emulator process name `pdp1`, working directory equal to `PDP1_ROOT` (normally `/opt/pidp1-mods`). The guarded drum helper uses these to identify the emulator.

The supplied preassembled program needs no compilation. Rebuilding additionally requires installed AM1 and its `TYPE340/type340alt.ah` and `DRUM/type23drumdefs.ah` headers. FFmpeg/ffprobe are needed only for importing another WAV.

## Install and check

1. Unpack into a new `SignalAnalysis` directory under `/opt/pidp1-mods/FunStuff/`. If that directory already exists, keep it separately before unpacking; do not merge an unknown older version into it. The resulting path should be `/opt/pidp1-mods/FunStuff/SignalAnalysis/workstation.rim`.
2. Open a terminal there and run:

   ```sh
   make check
   ```

   This only checks package hashes and the three sample-image formats/checksums; it does not operate the emulator. All files and sample images should pass.
3. Finish any current emulator session, turn power on, and halt the CPU. Disable debugger stepping/breakpoints/watches. Set Sense Switches 1, 2 and 3 off. The first load saves current core/registers, and selection saves the old drum. These backups are separate artifacts, not an automatic restoration of a whole interrupted session.
4. Select the fixed two-tone calibration and start:

   ```sh
   python3 drum.py pick samples/SignalAnalysis.drm
   make run
   make live-status
   ```

   `run` validates the selected signal image, saves a core snapshot and loads/starts the program. It refuses to load over a running CPU. No drum selection or program load occurs merely by unpacking the files.

For a different installation path, keep the directory name `FunStuff/SignalAnalysis` beneath that root. Pass `--root /your/pidp1-mods` to `drum.py` and `PDP1_ROOT=/your/pidp1-mods` to make, for example `make PDP1_ROOT=/your/pidp1-mods run`. The status/loader ports are fixed at 1040/1044 in this preview.

## What to check

- With all three switches off: a held waveform and two spectral bars at 512 and 1024 CPS, labeled X4. Their heights should be approximately 127 and 32 display units (the second has one quarter of the power). Status should show `running: true`, `done: 1`, `fault: 0`, `fft_overflows: 0`, and the published-frame count should remain steady. Working power can lead the displayed frame during continuous analysis.
- Check `display_timing_ms` in status. It records completed display-pass intervals independently of FFT frame analysis, with minute-wrap handling. Aim for at most 25 ms per pass; a nonzero `over_25ms` counter means that target was missed. The v2 scan model predicted up to 46.12 ms for steep speech waveforms; v3 reduces that to 23.15 ms across all 144 frames plus wraparound. This is a nominal hardware-model result, not a guarantee against host delays or other recordings. The optimized drawing plus bounded DMA stayed within 11–25 ms over a 75-second live test including two track changes. See `docs/VALIDATION.md` for measurement scope and later checks.
- Turn Switch 3 on: label becomes X64 and the strong calibration bars clip at the plot ceiling. Turn it off: original heights return. The held frame number stays unchanged.
- Pulse Switch 2 on/off: advance exactly one frame. Turn Switch 1 on: frame numbers advance and wrap through the recording. Turn it off: the display keeps refreshing on the held frame.
- Try `samples/SignalAnalysis-Changing.drm`: hold, halt, select using `drum.py pick`, then `make run`. In continuous mode, frames 00–03 contain 256 CPS; 04–07 contain 512 CPS; 08–11 contain 512 and 1024 CPS; 12–15 contain 1024 CPS alone. Use X4 to compare their powers.
- Try `samples/Apollo11.drm` the same way. It holds about nine seconds of Armstrong's speech across nine tracks. Use Switch 1 on and Switch 3 on. Waveforms and spectra vary; analysis takes longer than nine wall-clock seconds. Some nearly silent frames may still have no visible peaks. The analyzer does not play sound.

Record the installed revision, what appears on screen, and the output of `make live-status` if something differs. The latest loader result is `live-results.json`; core/register backups are under `snapshots/`. Recordings and permanent `Before-SignalAnalysis-...drm` backups are not deleted by the program. A fault halts execution; see README for the fault codes.

To leave the test, halt the CPU. To restore the prior drum, run `make drum-restore` while stopped. Then load the program appropriate to that drum; the helper does not resume the previous software or restore saved core automatically.

The full source is included. Start reading with `docs/CODE-TOUR.md`; comments beside `fastwave`, `fastbar` and the guide generator explain the scan-cost optimization and its dot-spacing tradeoff.

## Try the named Microtape source

After the drum checks, halt the CPU and run `make run-tape`. This validates and selects the supplied image on drive 8 with write-locking, preserving other drive assignments. Allow time for the loading scene; the first live Apollo run on Phillip's Pi took about 40 seconds with the final chunked loader. The top-right label should read `TAPE 8 APOLLO11`, status should report source tape, stage 9, fault 0 and no FFT overflows. Analysis then uses RAM and the tape remains stopped.

Check hold/step/gain as before. Halt and use `make run` to return to the unchanged drum source. See `docs/MICROTAPE.md` for preparation, the 11-second RAM limit, error recovery and changing names (which requires an assembler rebuild).

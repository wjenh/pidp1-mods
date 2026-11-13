# PDP-1 Signal Analysis Workstation

Experimental preview v4, 10 November 2026. Reads recorded samples from a Type 23 drum, runs a 256-point fixed-point FFT on the PDP-1, and displays a connected waveform and power spectrum through the Type 340. Analysis is slower than the original recording; this is not live audio playback.

Start with [Bill's setup and test guide](FIRST-TEST.md). Read [Microtape loading](MICROTAPE.md) for the new read-only tape source. Source navigation and calling conventions are explained in [the code tour](CODE-TOUR.md). A preassembled program and its complete workstation source are included. The development test suite and throwaway builds are omitted from this distribution. Validation results are summarized in `docs/VALIDATION.md`.

## Controls

| Switch | Off | On |
|---|---|---|
| 1 | Hold the current frame; display keeps refreshing | Analyze continuously, looping through the recording |
| 2 | Rearm single-frame stepping | Advance once while Switch 1 is off; turn off before another step |
| 3 | X4 spectrum gain | X64 gain for quiet peaks |

Gain changes redraw a held frame without changing its FFT or measured power. Strong peaks clip at the plot ceiling. The waveform gain is fixed. Long lines use wider beam-dot spacing to shorten the scan; all 256 sample endpoints and exact bar heights are preserved. The faint horizontal guides are dotted. The display label identifies the spectrum gain. The viewer window may still have a Type 30 title while the program uses the Type 340 controller.

## Normal operation

From `/opt/pidp1-mods/FunStuff/SignalAnalysis`:

```sh
make live-status
```

With the CPU halted, `make run-tape` loads the configured named recording from drive 8 into RAM; `make run` uses the selected drum image. The supplied tape defaults to APOLLO11. See `docs/MICROTAPE.md` for changing the recording.

To reload, halt the CPU with power on and no debugger stepping/breakpoints enabled, then run `make run`. It snapshots core/registers under `snapshots/` before loading and starting the program. Initialization is at octal `000400`. Hold mode deliberately leaves the CPU running for display refresh. Physical sense switches retain their settings.

To select a supplied recording, first turn Switch 1 off and wait for the held frame, then halt the CPU. For example:

```sh
python3 drum.py pick samples/Apollo11.drm
make run
```

The helper refuses a running CPU, waits for the active drum file to close, and backs up the current image before replacing it. It records a restore point in `drum-state.json`. `make drum-restore` restores that previous image while stopped; reload the appropriate program afterward. Do not operate another drum picker or Start/Continue concurrently with a selection.

## Files

- `workstation.rim` and `workstation-tape.rim` (with their `.sym`, `.lst`, `.map` files): assembled programs, symbols, listing and memory map. Live status requires the matching `.sym` file.
- `workstation.am1`, `*.ac`, `generated/`: assembly source and generated tables. `display.ac` handles display-completion interrupts and timing.
- `live.py`, `drum.py`: loading/status and guarded drum selection. `tape.py` configures and prepares named recordings; `tape-loader.ac` performs read-only guest loading.
- `import_wav.py`: optional WAV importer; needs FFmpeg and ffprobe. Run `python3 import_wav.py --help` for options.
- `samples/`: three standalone drum images plus the new SignalStation.mtp named-recording tape. No recording is selected merely by unpacking the package.
- `docs/`: design, integration notes and validation summary. Historical development notes describe test tools not included here.
- `audio/README.md`: NASA source and conversion credit. The original WAV can be downloaded from the linked source; it is omitted from this package.
- `SHA256SUMS`, `verify_package.py`: check the distributed files and sample-image checksums with `make check` before rebuilding or editing.

Display lists end with HALT + interrupt. A bank-0 sequence-break handler restarts the display independently of FFT/main-loop progress, and acknowledges a completed replacement list only at a stopped-frame boundary. It preserves the interrupted AC, IO, overflow flag and extend mode. Channel 0 belongs to this handler; unrelated break channels are disabled at startup. The BBN clock supplies millisecond completion intervals, reported by `make live-status` as `display_timing_ms` (last/min/max, completed passes and passes over 25 ms). The clock wraps each minute and the handler accounts for it. Measurements are integer run-clock milliseconds, not a hard wall-clock scheduling guarantee.

Track-cache fills use 128-word read-only DMA requests, with a display pass allowed between requests. This limits interference with display scans; loading a new track takes longer than one full-track request. Whole-track checksum verification still finishes before its samples are analyzed.

Optional development test sources are included under `tests/`, with targets in `Makefile.tests`. On the installed Pi, `diagnostics/` and `snapshots/` retain local history; neither is distributed to other machines.

## Limits and faults

Standard sampling is 4096 samples/second, 256 samples/frame, 62.5 ms/frame and 16 CPS/bin, displaying DC through 2048 CPS. Each sample track contains one second. The directory occupies track 0, leaving up to 31 seconds per image. A partial final track is zero padded. The current display does not distinguish padding with an on-screen valid-sample indicator.

No windowing, automatic normalization, previous-frame navigation or light-pen control is implemented. Remaining quiet frames may contain insufficient energy or values lost during fixed-point truncation.

Tape faults 6–8 display a refreshing error scene and report a stage/status; see `docs/MICROTAPE.md`.

Fault word octal `000102`: 1 = header; 2 = drum status/timeout; 3 = checksum; 4 = display edge; 5 = list capacity. Faults halt the CPU. Inspect the cause and reload rather than repeatedly continuing.

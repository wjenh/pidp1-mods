# Read-only Microtape recordings

Version 4 adds named recording loading using Bill's FILESYSTEM/MICROTAPE 2.1 library. The PDP-1 mounts the filesystem, opens the selected file and reads it through the emulated Type 550 into extended RAM. It validates the complete recording before starting the unchanged FFT and power calculation. No tape I/O takes place during analysis.

The first supplied image, `samples/SignalStation.mtp`, contains three read-only files:

| Name | Recording |
|---|---|
| `APOLLO11` | Armstrong speech, 36764 valid samples, nine logical seconds |
| `TWOTONE` | One second of the 512/1024 CPS calibration |
| `CHANGING` | One second of the four-part changing calibration |

The default is **drive 8 / APOLLO11**. File names are limited to nine characters and are case-insensitive; the station's configuration tool accepts letters, digits, dot, underscore and hyphen.

## Start and change recordings

With power on, the CPU halted and the display viewer open:

```sh
cd /opt/pidp1-mods/FunStuff/SignalAnalysis
make run-tape
```

The launcher validates/extracts the selected file with the host `mtfs` tool first, snapshots core, then selects the image on the configured drive with `,locked` in `microtapes.txt`. It preserves other drive assignments and saves the old assignment file under `snapshots/` before changing it. It refuses to replace a drive assigned to another image. It then loads the tape-start binary. All tape movement and sample loading are performed by the PDP-1 program, not by a host memory upload.

A paced looping `LOADING APOLLO11` scene refreshes without interrupts while the tape is read. An early single-read prototype took approximately 15 seconds; the final chunked loader deliberately spends longer to recover occasional missed transport words without repeating the whole recording. Its verified live Apollo load took about 40 seconds, including the first FFT. Actual time depends on tape position and retries. Allow up to three minutes before the launcher declares a timeout. A new run starts from the beginning; it reloads the recording rather than trusting samples left in RAM.

The ordinary controls remain: Switch 1 run/hold, Switch 2 next frame, Switch 3 X4/X64 power gain. The source label shows the tape drive and recording. Track/frame labels refer to logical seconds and their sixteen FFT frames; the tape stays stopped during analysis. The last logical second is zero-padded as in the drum version.

To select another supplied recording, halt first:

```sh
python3 tape.py configure --name CHANGING
make run-tape
```

This rebuild requires AM1 and Bill's current include files. Configuration defaults to drive 8 and the supplied image. Use `--drive N --image PATH` when configuring another image; specify all non-default settings again. Relative image paths are relative to `PDP1_ROOT/Microtapes/`; absolute paths are also accepted. The default portable path is `../FunStuff/SignalAnalysis/samples/SignalStation.mtp`.

To return to the selected drum recording, halt and run:

```sh
make run
```

That selects the drum-start binary; it does not copy or replace the drum image. Changing source does not change the physical sense switches. Core snapshots and tape assignments are separate from recording media, and no whole-machine session is restored automatically.

## Prepare a new recording

Existing workstation drum images can be packed without another conversion:

```sh
python3 tape.py pack /path/to/new.img \
  --recording MYCLIP=/path/to/myclip.drm \
  --tool /opt/pidp1-mods/bin/mtfs
```

The command creates a new image exclusively, marks files read-only, verifies extraction byte-for-byte, and publishes the image as a read-only host file. It refuses existing output names. It does not select a drive or modify a mounted tape.

The WAV importer can also produce a recording word file directly:

```sh
python3 import_wav.py /path/to/input.wav --duration 10 \
  --format tape --output /path/to/myclip.words
python3 tape.py pack /path/to/new.img \
  --recording MYCLIP=/path/to/myclip.words
```

WAV decoding, channel selection, explicit gain and anti-alias resampling are unchanged. There is no host FFT or automatic normalization. Use the existing importer options for channel/gain/excerpt selection. Microtape RAM loading currently accepts **at most eleven logical seconds**, even though the generic filesystem can hold larger files.

## Recording container v2

All fields below are word indexes in decimal. Each word is stored in a little-endian 32-bit slot containing only its low 18 bits. The file begins with 256 header words, followed by complete 4096-word logical seconds of samples.

| Header word | Value |
|---|---|
| 0 | Magic `0615327` (octal) |
| 1 | Container version 2; distinct from the raw drum directory version 1 |
| 2 | Sample rate 4096 |
| 3 | FFT length 256 |
| 4 | Valid sample count, positive and within the last logical second |
| 5 | Padded frame count: a positive multiple of 16, at most 176 |
| 6–9 | Fraction bits 17, input guard 1, FFT stage shifts 8, flags 0 |
| 10 | Signal identifier |
| 11 | Sum of all padded sample words modulo 262143 |
| 12 | Header checksum: all 256 header words sum to zero modulo 262143 |
| 13–255 | Zero |

The file length must be exactly `256 + frame_count*256` words. Samples retain the existing one's-complement signed fractional representation. Unused tail samples must be canonical positive zero. The filesystem adds its own block checksums independently. File information type/application values are left at zero in the supplied image; the application header identifies the format.

The loader allows five attempts for a device-level bad-block/missed-word read, seeking back to the original descriptor position before retrying. A persistent error fails after the fifth attempt; application header/checksum/padding errors are not retried. `make live-status` reports the number of tape read retries.

The station checks device/filesystem status, the actual count returned by `fsRead`, all header fields, file length, payload checksum and padding. A successful short read is treated as an error. An unsupported/oversized recording is rejected before reading into the sample RAM region.

## Safety and error behavior

The guest issues no image-creation mount (`MMT`), format, file create, write, delete or pack calls. The launcher requires an existing image and makes a write-locked drive assignment. `mtMount`, `fsOpenName`, `fsRead` and unchanged-descriptor `fsClose` are the loading path. Transitive library code contains unused write routines; the station does not invoke them.

The image remains mounted, stopped and write-locked on drive 8 after loading. Existing drive 1/2 assignments on Phillip's machine remain intact. To stop using the station's tape assignment later, halt the CPU and remove only its drive line through the ordinary tape management interface. Do not edit a tape image with host tools while a guest has a live filesystem handle open.

Tape failures show `TAPE LOAD FAILED / CHECK TAPE AND FILE NAME`, and keep that scene refreshing. No FFT is started with invalid samples. Halt before correcting/reloading. `make live-status` reports:

- `source`: `tape` or `drum`.
- `tape_stage`: 2 mount, 3 open, 4 header read, 5 validation, 6 payload read, 7 close, 8 payload/padding check, 9 ready.
- `tape_status`: the filesystem's status code, printed in octal in launcher errors.
- `fault`: 6 device/filesystem error, 7 invalid header/length, 8 short read or bad payload/padding. Original drum/display faults 1–5 retain their meanings.

The CPU continues running to refresh a tape-error scene, unlike the original faults which halt. Host preflight may reject a bad file before the guest program is loaded, leaving the prior stopped session unchanged apart from its saved snapshot, if one was already taken.

## Implementation and memory

The filesystem library disables sequence breaks around block movement and clears pending breaks when restoring them. We call it only during startup, with breaks already disabled. The loading/error scenes loop natively in field 1, within the Type 340's 13-bit jump range. The library is never called from the normal display handler. After successful validation the loader stops the loading scene, and the ordinary first-scene publication explicitly restarts DLA, clears stale requests and enables the normal handler with fresh timing counters.

Fields 0–3 retain the numerical/display/drum layout. Field 4 contains configuration, the loader, header and filesystem state; the complete assembled field ends at octal `045640`, using 2976 words including reservations. Fields 5–15 hold up to 45056 sample words. `acquire` copies one RAM frame into the same input array used by the drum path. The numerical kernel and frozen truncation/scaling contract are unchanged.

The filesystem needs PDP-1D instructions (`core1D` or `all1D`). The existing original-instruction-set kernel tests remain separate; tape integration tests enable the extensions. The tested Pi has both enabled. See `CODE-TOUR.md` for the rest of the program structure.

# PiDP-1 Signal Analysis Workstation

Engineering specification, 10 November 2026. The first drum/Type 340 workstation is now implemented, target-tested and running; see `target-preflight.md` for the installed-target audit and integration limits.

Decisions 1 and 2 are inherited from the preceding discussion. Decision 3 is frozen here as the numeric contract. Decisions 4-8 below establish the implementation baseline. Addresses and six-digit machine words are octal; counts, frequencies, and times are decimal unless explicitly marked otherwise. Memory allocations are budgets, not measured assembled sizes. Timing is an estimate, not a benchmark.

## 1. Frozen machine and sampling assumptions

- PDP-1 one's-complement arithmetic, automatic Type 10 multiply/divide, extended core, Type 23 drum, **Type 340 display**. On 10 November the user replaced the initial Type 30 requirement with Type 340; the numeric and sampling contract is unchanged.
- Prerecorded real samples; no ADC in version 1. The PDP-1 performs all analysis and display generation.
- 4096 samples/second; one signed 18-bit sample per drum word.
- 256 samples/frame, 62.5 ms of signal/frame, 16 frames/track.
- 4096 words/track, representing one second of signal. This is a data convention; it is unrelated to the drum's physical rotation time.
- FFT spacing 16 CPS; bins 0 through 128 inclusive cover DC through 2048 CPS: **129 bins**. Bins 1-127 have negative-frequency partners; DC and Nyquist do not.
- Rectangular window initially. Retain DC. Do not silently subtract the mean, apply a taper, or change the signal gain.
- Light-pen interaction and other FFT lengths are later extensions.

## 2. Evidence and authority

The project `sources/PDP1.md` is useful guidance but includes illustrative code and simplified claims; it is not the final authority for arithmetic or device programming. For example, its TTY example confuses sense-switch and program-flag tests. Do not copy examples without checking their instruction semantics.

Local DEC references consulted:

- `sources/MACRO-PDP-1_Macro.pdf`, printed pp. 8-9 and 13-15: octal integer words, location assignments, tags, and explicit constant/variable areas. The original MACRO specification concludes the source with START; it does not justify putting START at the beginning as in some local examples.
- `sources/DDT--DEC.pdp_1.1964.102650078.pdf`, main introduction and Extend DDT-1 appendix (PDF pp. 26-29): ordinary DDT occupies 6000-7750; its symbol table consumes additional space below it. Extend DDT begins at 4600 in its selected module, may use 7765-7777 elsewhere, and its loader occupies 7751-7777.
- `sources/ET-manual-DEC.pdp_1.1972.102650079.pdf`, PDF p. 2: ET uses drum field 2 for text, field 1 normally holds binary programs, and extra text can consume subsequent fields. This describes that ET environment, not a guarantee that the local ET binary uses the Type 23 interface.

Hardware cross-check: [DEC PDP-1 manual, automatic multiply/divide option](https://gordonbell.azurewebsites.net/Digital/PDP%201%20Manual%201961.pdf). The instruction timings and split product layout below come from the hardware description; the FFT design and bounds are derived here.

Implementation cross-check: public [wjenh/pidp1-mods](https://github.com/wjenh/pidp1-mods) at commit `dbad12b3e68cee321838befb1d685a16e05d0155`, retrieved during this review:

- [CPU implementation](https://github.com/wjenh/pidp1-mods/blob/dbad12b3e68cee321838befb1d685a16e05d0155/src/pdp1/pdp1.c): `multiply`, `mul_shift`, and `shro`.
- [Type 23 usage](https://github.com/wjenh/pidp1-mods/blob/dbad12b3e68cee321838befb1d685a16e05d0155/Docs/UsingType23Drum.md) and its IOT handlers.
- [Type 30 handler](https://github.com/wjenh/pidp1-mods/blob/dbad12b3e68cee321838befb1d685a16e05d0155/IOTs/Type30Display/IOT_7.c).
- [Extended-memory access helpers](https://github.com/wjenh/pidp1-mods/blob/dbad12b3e68cee321838befb1d685a16e05d0155/Am1Includes/memory.ah).

The initial design used the public sources. A subsequent target audit checked installed revision `53871ec0450f24f97ac178e016350c85fe1fce09`, AM1 v5.7, 64K core, multiply configuration, and installed IOT modules; see `target-preflight.md`. The normal live emulator was not altered.

## 3. Frozen numeric contract

### 3.1 One's-complement fractions

Let D = 2^17 = 131072. Decode a machine word w as the signed integer d:

- If w < 0400000, d = w.
- Otherwise d = -(0777777 XOR w).
- Its fractional value is d/D.

This is a sign bit and 17 fractional positions in one's-complement representation. It is not a two's-complement Q17 encoding, and the divisor is **131072, not 131071**.

| Value | Machine word |
|---|---|
| +0 | 000000 |
| -0 | 777777 |
| +1/131072 | 000001 |
| -1/131072 | 777776 |
| +1/2 | 200000 |
| -1/2 | 577777 |
| +(1-2^-17) | 377777 |
| -(1-2^-17) | 400000 |

Neither +1 nor -1 is representable. In particular, 400000 is the most-negative fraction, not -0. Do not apply two's-complement absolute-value, negation, or overflow rules.

Normalize both zero encodings to +0 at input and when publishing results. Internal negative zero is allowed; numerical zero tests must accept both encodings. A truncated negative product can leave -0 in AC even when the complete AC:IO product is nonzero.

Drum samples retain their full range. Only the FFT working copy is shifted right once for a guard bit. Preserve original samples for the waveform.

### 3.2 Multiply conversion

For integer magnitudes A and B, let P = |A B|. A positive automatic MUL result has:

    AC = floor(P / 2^17)
    IO = (P mod 2^17) << 1

The product sign is in AC bit 0 and IO bit 17. For a negative product the stored result is complemented. The useful 34 product positions run from AC bit 1 through IO bit 16. IO is not an ordinary signed low word.

For two fractional operands, **AC already holds the fractional product truncated toward zero**:

    M(a,b) = trunc_toward_zero(a*b / D)       [signed integer codes]

No subsequent shift is needed for the one-word result. Discard IO for FFT multiplication. Hardware MUL destroys IO, so no live pointer or loop state may reside there across a multiply. Require automatic MUL mode; MUS is the same opcode interpreted as multiply-step when the option is disabled.

Example: +1/2 times +1/2 gives AC = 100000, the fraction +1/4. With one negative operand AC = 677777. These two cases should be the first DDT checks on the target.

DIV is not needed in the FFT. Powers of two use arithmetic shifts; frequency labels use multiplication by 16.

### 3.3 Scaling and butterfly evaluation order

Define H(v) = trunc_toward_zero(v/2). PDP-1 SAR 1S implements H on one's-complement codes, including preservation of -0. It does not round negative numbers downward as a modern two's-complement arithmetic shift does.

Load the real data as H(sample), imaginary data as zero. At every radix-2 DIT butterfly, calculate:

    tr = M(br, wr) - M(bi, wi)
    ti = M(br, wi) + M(bi, wr)

    ar2 = H(ar);  ai2 = H(ai)
    tr2 = H(tr);  ti2 = H(ti)

    out0 = (ar2 + tr2, ai2 + ti2)
    out1 = (ar2 - tr2, ai2 - ti2)

Here wi is the negative sine for a forward transform. Save the halves before overwriting either input.

**Do not compute ar+tr or ar-tr before shifting.** The contract is the evaluation order above; H(a)+H(t) and H(a+t) can differ by a count.

Special twiddles are exact operations:

- W^0 = +1: t = b, with no multiplication.
- W^64 = -j: tr = bi, ti = -br, using one's-complement negation and boundary zero normalization.

There are eight stage halvings plus the initial guard-bit shift. Ignoring quantization, the stored fractional output is:

    Y[k] = X[k] / 512
    X[k] = sum over n of x[n] exp(-j*2*pi*k*n/256)

Thus **X[k]/256 = 2Y[k]**. Record this gain explicitly; do not label Y as X/256. A real, bin-centered sinusoid of peak amplitude A has |Y[k]| approximately A/4 at its positive-frequency bin. DC of value A gives Y[0] approximately A/2.

### 3.4 Why it stays within range

This proof is for the frozen real-input contract, not arbitrary complex input of independently full-scale real and imaginary components.

After the initial shift, each complex input has norm below 1/2. With exact unit twiddles, (a +/- Wb)/2 preserves that bound by the triangle inequality.

Generate table components by truncating their magnitudes toward zero; then |W_quantized| <= 1. Each truncated scalar multiplication has error below q = 2^-17. Computing tr and ti introduces complex error below 2*sqrt(2)*q. Halving a and t separately adds less than 2*sqrt(2)*q of complex error, while halving the multiplication error contributes less than sqrt(2)*q. Consequently:

    B_next <= B + 3*sqrt(2)*q
    B_after_8 < 1/2 + 34q < 0.500260

For stored inputs of norm at most B, even a conservative component-wise bound on the unhalved rotation is sqrt(2)*B + 2q, below 0.708. Each final component is bounded by (B + sqrt(2)*B + 2q)/2, below 0.605. The product subtractions and final additions therefore stay well inside the signed range. No saturation, wrapped overflow, or data-dependent exponent is required.

The extra guard bit makes the simple four-multiply implementation safe without a double-word butterfly accumulator. It costs one input bit. Fixed stage scaling also reduces small-signal precision; version 1 accepts that cost for a reproducible gain and simple code. Block-floating scaling is a later algorithm change, not an undocumented optimization.

## 4. FFT organization and twiddle tables

Freeze a 256-point, in-place, radix-2 decimation-in-time complex FFT, with separate 256-word real and imaginary arrays. Load real samples into bit-reversed locations; output bins are in natural order. Zero all imaginary words before each transform.

For stage lengths m = 2, 4, ..., 256, the twiddle index is j*256/m for j = 0 through m/2-1. Each stage has 128 butterflies, for 1024 total.

Store 128 cosine words and 128 negative-sine words in the FFT field. For 0 <= k < 128:

    C[k] = signed_trunc(D*cos(2*pi*k/256))
    S[k] = signed_trunc(-D*sin(2*pi*k/256))

Clamp the unrepresentable cardinal components to +/-131071 in the stored table, but bypass their multiplies as specified above. Force mathematical zeros to +0. Table generation must handle cardinal angles explicitly rather than trusting floating-point residuals.

| k (decimal) | Cosine word | Negative-sine word | Execution |
|---:|---|---|---|
| 0 | 377777 | 000000 | Exact copy |
| 1 | 377730 | 771557 | Multiply |
| 32 | 265011 | 512766 | Multiply |
| 64 | 000000 | 400000 | Exact -j rotation |
| 127 | 400047 | 771557 | Multiply |

Spend the 256 words. Do not generate trigonometric coefficients at runtime or compress the table in version 1. Host-generated mathematical constants are permitted; host-side signal analysis is not part of the running workstation.

There are 255 unity butterflies and 127 -j butterflies. Specializing them leaves 642 general butterflies and 2568 scalar MUL instructions, versus 4096 for an unspecialized implementation.

Reserve 256 words for future window coefficients. The initial rectangular window is a bypass, not multiplication by the largest representable fraction. A future Hann mode must define periodic versus symmetric coefficients, coherent gain, and power calibration before activation.

## 5. Core memory map

The implemented workstation uses four operating fields (16384 words): field 0 for the numerical engine, field 1 for directory/power/publication state and the display driver, field 2 for two display lists, and field 3 for a complete drum-track cache. The cache is an implementation refinement that enables whole-track checksum validation and subsequent frame reads without more DMA. The FFT remains within one 4K field. This build uses AD1; any future Extend DDT build needs an alternate cache layout that preserves its cross-module bank-end scratch.

Field 0 is the numerical engine:

| Local octal range | Words (decimal) | Assignment |
|---|---:|---|
| 0000-0377 | 256 | Sequence-break/CAL reservations, gateways, control state |
| 0400-1777 | 768 | FFT and arithmetic code, including explicit literal pools |
| 2000-2377 | 256 | Real array |
| 2400-2777 | 256 | Imaginary array |
| 3000-3177 | 128 | Cosines |
| 3200-3377 | 128 | Negative sines |
| 3400-3777 | 256 | Future window table |
| 4000-4377 | 256 | Input staging |
| 4400-4777 | 256 | Arithmetic temporaries and counters |
| 5000-5777 | 512 | Code expansion or ordinary-DDT symbol space |
| 6000-7777 | 1024 | Debugger/loader reservation |

The initial main entry is 0400. In a normal build the two code regions allow up to 1280 words, but a base-DDT build must account for its downward-growing symbol table. If symbols exceed the reserved gap, use Extend DDT in field 2 or reduce the symbol set; do not overwrite live code.

Field 1 is I/O and display:

| Local octal range | Words (decimal) | Assignment |
|---|---:|---|
| 0000-0377 | 256 | Gateways, saved context, I/O state, pending power work |
| 0400-1377 | 512 | Validated directory/header and track-record snapshot |
| 1400-1777 | 256 | Published original waveform |
| 2000-2377 | 256 | Power high limbs; first 129 words used |
| 2400-2777 | 256 | Power low limbs; first 129 words used |
| 3000-3777 | 512 | Spare publication/workspace capacity |
| 4000-5777 | 1024 | Scheduler, display, drum, and user-control code |
| 6000-7677 | 960 | Type 340 list templates, labels, spare space; character shapes use Type 342 |
| 7700-7777 | 64 | Loader and cross-field debugger reservation |

The implemented display lists contain their own coordinates and bar heights. Publishing a new waveform does not modify the list currently being drawn. Track/frame identity is encoded in the same completed list before publication.

Field 2 is reserved for Type 340 display lists:

| Local octal range | Words (decimal) | Assignment |
|---|---:|---|
| 0000-0077 | 64 | Display publication/control reservation |
| 0100-3777 | 1984 | Display list A |
| 4000-7677 | 1984 | Display list B |
| 7700-7777 | 64 | Loader/cross-field debugger reservation |

The display driver builds only the inactive list and publishes its full address after the list and matching waveform/spectrum/frame identity are complete. Every list ends with an explicit halt and is bounded by an assembler/runtime length check. Initial word budget per list: about 512 waveform-position words, at most 774 for spectrum bar positioning and split vertical vectors, and at most 500 for parameters, axes, ticks and Type 342 labels, totaling no more than 1786 words before final alignment. These are budgets to check against the actual encoder, not measured sizes.

Field 3, local 0000-7777, is the 4096-word read-only track cache. It is filled by Type 23 DMA and checksummed on a cache miss. AD1 consumes no PDP-1 core. The full cache intentionally uses the bank end, so a future Extend DDT build cannot reuse this layout unchanged.

The implemented version reads full tracks into field 3 and copies the selected 256-word frame into field 0. The AM1 loader remains in the reserved top of field 0; no memory-resident debugger is loaded.

Use field:local notation in listings. Full extended address = field*010000 + local address. This is an address convention, not an extra bank field inside every instruction.

The steady-state FFT uses field-local addressing. Cross-field copies and transfers use deliberate EEM/LEM windows and 16-bit indirect addresses. Preserve return field, extend state, AC, and IO at scheduler boundaries. Do not apply an ordinary skip to an entire multi-instruction far-access macro. MACRO and AM1 have different helper syntax; select the installed assembler explicitly at implementation time.

## 6. Execution-time budget

DEC nominal instruction times: memory-reference operations 10 us, arithmetic shifts 5 us, automatic MUL 14-25 us. Indirection adds a memory cycle. These support an engineering estimate, not a claim about the eventual optimized instruction stream.

A direct-address general butterfly can be budgeted as:

| Work | Memory ops | MUL | SAR | Time |
|---|---:|---:|---:|---|
| Form and halve tr,ti | 14 | 4 | 2 | 206-250 us |
| Halve a; form/store both outputs | 14 | 0 | 2 | 150 us |
| Arithmetic subtotal | 28 | 4 | 4 | 356-400 us |

For example, each rotated component needs two LAC/MUL/DAC sequences followed by LAC, ADD or SUB, SAR, DAC. Each output component needs LAC, SAR, DAC, ADD, DAC, LAC, SUB, DAC. Address setup, loop control, coefficient selection, buffer transfers, scheduling, and checks are additional.

At 1024 generic butterflies, arithmetic alone costs about 0.365-0.410 s. Exact cardinal cases reduce this; indexing and scheduling increase it. Budget **0.5-0.8 CPU seconds per FFT** until assembly provides a defensible count. This is a conservative planning range, not a measured upper bound.

129 power bins require 258 squares plus two-limb accumulation, plotting conversion, and publication. Reserve another 20-40 ms of CPU time for those steps and about 10-20 ms for frame preparation; revisit from the actual listing.

The Type 23 parameters in the reviewed implementation are 250 us select settling, 8.5 us/word, and 4096 words/revolution. Therefore:

- Revolution: 34.816 ms.
- 256-word payload transfer: 2.176 ms.
- A frame read takes about 2.426 to 37.242 ms before software overhead, depending on rotational phase. There is also CPU contention from high-speed-channel transfers.
- Full-track payload transfer: 34.816 ms, not one second.

The 62.5 ms figure is signal duration, not allowable computation time. The workstation will analyze stored frames at a slower pace. All sixteen frames of a track can be examined in order without implying real-time playback. Do not promise sixteen transforms/second.

## 7. Display, power, and scheduling

### 7.1 Power definition

Keep all 129 nonnegative-frequency bins, including the Nyquist endpoint. Let R and I be signed integer output codes. Compute the exact nonnegative integer sum S = R^2 + I^2 from full MUL products. Do not square only the AC high word.

Store S as two ordinary, nonnegative 17-bit limbs:

    S = high*2^17 + low
    0 <= low < 2^17

For a positive square product, AC is the high limb and IO shifted right logically by one is the low limb. Add low limbs with an explicit carry into the high limb. Since the two 17-bit low limbs can sum past the signed maximum while still fitting in 18 physical bits, this addition is an unsigned-limb operation, not a signed fractional ADD with overflow ignored. These stored limbs are not the MUL AC:IO representation.

The normalized, two-sided bin power is:

    |X[k]/256|^2 = 4*S / 2^34

For a rectangular window the one-sided mean-square contribution is:

    P[k] = 4*S / 2^34             for k = 0 or 128
    P[k] = 8*S / 2^34             for 1 <= k <= 127

The sum of these P values approximates the frame's mean-square signal value by Parseval, within fixed-point error. A bin-centered sine of peak amplitude A has one-sided power A^2/2. DC A has power A^2. Do not double DC or Nyquist. Version 1 displays linear one-sided power, with explicitly indicated manual gain; logarithmic power, PSD per CPS, and amplitude readouts require additional calibration rules.

### 7.2 Type 340 autonomous display lists

The display target is now Type 340 with the installed Type 342 character generator. The Pi has IOT_15.so, IOT_16.so and IOT_17.so installed. Use the project's `Docs/UsingType340Display.md` and `Am1Includes/TYPE340` definitions as the implementation references.

The PDP-1 prepares a display list in core and starts it with DLA. The Type 340 fetches its own instructions through high-speed channel 3 while the CPU continues the FFT. Plot axes and spectrum bars as vectors, labels with the Type 342 character mode, and waveform samples as a connected vector polyline. Steep transitions are split into two or four valid vector segments while preserving endpoints.

Use unsigned 10-bit screen coordinates 0-1023, not the old Type 30 centered coordinate packing. Spectrum x(k) = 128 + 6k for k=0..128, with labels 0,512,1024,1536,2048 CPS. Place the waveform above the spectrum, with guard margins to avoid edge violations. Power heights retain the one-sided calibration in section 7.1. Choose an initial maximum bar height of 256 screen units; split vectors longer than their 7-bit delta limit of 127.

Use the full 16-bit address accepted by the installed DLA implementation to start either field-2 list. The Type 347 subroutine/jump address is only 13 bits; do not try to loop back to a field-2 list with a 347 jump. Instead end each pass with HALT, poll display completion at bounded FFT work boundaries, and issue DLA again for the current published list or a newly completed replacement. Initially keep light-pen detection and stop interrupts disabled; use named Type 340 status operations and error checks. DCF clears flags but does not stop the display. A list halt is the publication boundary; do not modify a list while the 340 owns it.

The reviewed timing reference gives a 5 us high-speed-channel fetch for every instruction, a 35 us point-position delay, and length-dependent vector/character timing. For point positioning, issue an invisible Y update before X to avoid two deflection waits. Vectors require about 1.5 us per rate-multiplier step plus setup; they are not constant-time endpoints. Character rendering also has variable cost. The largest speed benefit to the CPU is autonomous drawing and use of vectors/characters rather than executing DPY for every dot. Display fetches still steal CPU memory cycles, so include their contention in the FFT benchmark.

Target roughly 20-30 refresh passes/second for the initial measured scene, adjusting from actual list duration and CPU contention. A 256-point waveform alone needs about 11.5 ms with two point words per sample and the correct Y/X ordering; spectrum repositioning, vectors, text, and polling latency are additional. Do not promise this refresh rate for a worst-case scene with all tall bars or claim 16 analyzed frames/second: the measured numerical core still takes about 0.768 CPU seconds without display/drum contention.

Poll/restart the display after at most four general butterflies, preserving the FFT's loop state. This limits completion-to-restart delay to a few milliseconds without making every point a CPU operation. Build the next display list off-screen, and switch only when it contains a matching waveform, spectrum, frame identity, and calibration state. Existing results remain visible while the next FFT runs. The initial display and hold/run/next controls are implemented. The measured hold-mode refresh is about 25 passes/second on the Pi; the first-frame isolated test takes about 2.05 simulated seconds including validation. Additional interactive controls and gain selection are later work.

The Type 23 and Type 340 headers both use DRA/DSS names. Include the alternate Type 340 definitions and call the explicit `t23dra`, `t23dss`, `t340dra`, and `t340dss` names. The installed definitions provide those aliases; avoid ambiguous bare names. Type 340 stop/light-pen interrupts use SBS channel 0 if enabled, so their later introduction must be coordinated with the drum's sequence-break behavior.

Implemented controls are hold/continuous via Sense Switch 1 and one-frame advance via a Sense Switch 2 on/off pulse. Display gain is fixed at X4 for one-sided power. Track/frame selection menus, previous-frame operation, gain adjustment, and light-pen interrogation remain later extensions. The first live scene is the 512/1024 CPS calibration recording.

## 8. Drum format and interface

### 8.1 Logical storage

Freeze version 1 as one recording per dedicated 32-track application image. Track 0 is metadata; tracks 1-31 are samples, giving up to 31 seconds. Track numbers are decimal in this paragraph; machine values remain octal. If sharing an image later, introduce an explicit allocation map rather than assuming any existing track is free.

Every sample track has exactly 4096 fractional sample words in chronological order and no embedded header. Frame j starts at drum word j*0400 octal, for decimal j = 0..15. Global sample index is (track-1)*4096 + j*256 + n. Nominal signal time is that index/4096 seconds.

Metadata belongs on its own track so a sample track remains one second and sixteen frame slots. A final partial track is zero padded; metadata identifies its valid sample count and the UI marks padded frames. Padding is digital silence (+0), not stale memory.

Directory header, local octal offsets 0000-0037, contains unsigned 18-bit integer words (these fields are not fractional samples):

| Offset | Contents |
|---|---|
| 0000 | Magic 615327 |
| 0001 | Format version 1 |
| 0002 | Sample rate 010000 (4096 decimal) |
| 0003 | FFT length 000400 (256 decimal) |
| 0004 | Number of sample tracks, 1-31 decimal |
| 0005 | Total valid sample count, at most 126976 decimal |
| 0006 | Fractional-bit count 000021 (17 decimal) |
| 0007 | FFT input guard shifts, 1 |
| 0010 | FFT stage shifts, 000010 (8 decimal) |
| 0011 | Analysis flags: 0 = rectangular, DC retained |
| 0012 | Signal ID, unsigned application number |
| 0013 | Directory checksum |
| 0014-0037 | Reserved, zero |

Starting at 0040, reserve 31 consecutive eight-word records, one per sample track:

1. Physical track number within this dedicated image, 1-31 decimal.
2. First global sample index.
3. Valid sample count on this track, 1-4096 decimal.
4. Track checksum over all 4096 words, including padding.
5. Track flags, initially zero.
6-8. Reserved, zero.

Unused records and the rest of the directory track are zero. All but the final used track must have 4096 valid samples. Require contiguous physical tracks and matching sample indices in version 1.

Checksums use raw unsigned 18-bit word values, summed modulo (2^18-1). A sample-track checksum is the residue itself. The directory checksum is the additive inverse of the other directory words, modulo (2^18-1), so the whole directory sums to zero. Store a zero checksum as +0. This is an accidental-corruption check, not a cryptographic checksum.

Persist samples and metadata only. FFT and power results remain in core in version 1. Sample data and all padding zeros are canonicalized before checksumming.

### 8.2 Machine I/O

Use DIA, DWC, DCL for read-only full-track transfers into the field-3 cache, polling completion:

- DIA (device 61): IO bit 0 enables read; bits 1-5 choose the sample track; bits 6-17 select the word offset.
- DWC (device 62): leave write-enable bit 0 clear; the implemented count is zero for 4096 words. A zero count means a full track, not zero words.
- DCL (device 63): core bank in IO bits 2-5, buffer address in bits 6-17; starts the asynchronous transfer. The high-speed channel uses the full core address independently of CPU extend mode.
- Poll CKS IO bit 17: 1 is busy. After completion, use DRA = 722062 to check the returned error indication. Do not use the erroneous 723062 spelling warned against in the current implementation documentation.

Use DIA rather than DBA to avoid the latter's additional address break. Keep sequence breaks disabled in the first polling build. The reviewed implementation still queues an end break; clear pending breaks before a later transition to enabled sequence-break mode. Define that transition deliberately rather than introducing it as an incidental display optimization.

The cache is not read or modified while the drum owns its DMA transfer. At most one transfer is active. Verify DRA status and the whole-track checksum before copying a frame. Display lists have separate active/inactive ownership; the isolated tests check that an active list is unchanged until its pass completes.

Do not place a breakpoint or halt during a transfer: the reviewed implementation treats resume after a partly completed transfer as an incomplete-transfer error. Stop scheduling I/O and wait for idle before debugging its consumer.

### 8.3 Host container and development coexistence

The logical format above is independent of the host file representation. The reviewed implementation uses uint32_t Word and reads/writes arrays of those words directly. On a little-endian Raspberry Pi the existing container is therefore 32-bit little-endian slots with the low 18 bits meaningful, not densely packed 18-bit words. Validate the installed implementation before emitting such a file; use a portable octal-word interchange representation if moving data between different hosts.

**Target-audit correction:** the Type 23 module uses the fixed active path `/opt/pidp1-mods/pdp23drum`, while pdp1central already supplies controlled image switching through its Drums library. Create `Drums/SignalAnalysis.drm`; reach an idle point with no transfer active and halt the machine, Save drum under a durable name, then Pick the workstation image before loading/starting its program. Pick saves the previous active image to `pdp23drum.bak` and the plugin reloads the replacement on its next transfer. Pick does not automatically halt or synchronize pending writes; a later Pick replaces `.bak`. Restore the prior named image through the same idle/halted workflow. No emulator or configuration changes are required, and switching is implemented by the workstation helper described in its README. Do not overwrite an existing pdp23drum or assume its tracks are disposable. ET's documented drum use and the example PiDP-1 image's program tracks both demonstrate why a fixed guessed track allocation is unsuitable. ET, MACRO, and the analyzer do not run concurrently in version 1.

A future WAV importer must resample and low-pass appropriately before producing 4096-sample/second data. That preparation is permitted; the workstation's FFT, power, peak selection, and display remain PDP-1 computations. Record any importer gain in separate provenance; do not silently normalize every frame.

## 9. Design checks completed and implementation acceptance

Scratch calculations, without writing workstation code, checked:

- Every signed integer code -131071 through +131071 (262143 values): one's-complement sign-preserving right shift agrees with truncation toward zero.
- Positive/negative half-scale products, full-scale products, and tiny negative products: decoded AC agrees with signed truncation of a*b/2^17, including the possibility of negative zero.
- Table magnitude contraction, and the octal coefficient examples above.
- All eight FFT stages for full-scale positive DC, negative DC, alternating Nyquist samples, an impulse, a bin-32 sine, a 31.3-bin off-bin sine, and ten deterministic random full-scale sign sequences.
- Comparison against a direct floating-point DFT divided by 512. Largest complex output error in those 16 frames was 7.554 stored integer counts; DC and Nyquist errors were 1.5 counts. Expected peak bins were present. No checked intermediate exceeded 65535 in magnitude; the analytical proof covers larger valid rotations than those particular examples exercise.

These are numerical-design checks, not assembled-code or target-emulator tests. They do not establish cycle counts or installed-device compatibility.

Before accepting implementation:

1. Confirm installed source revision, automatic multiply mode, at least two memory fields, selected assembler, and IOT modules. Resolve loading into field 1 and the optional debugger field using the actual loader.
2. Under DDT, verify MUL half-scale cases, SAR on negative odd values, both zeros, and exact +/-j rotations. Check that scalar routines preserve their documented state and the overflow latch is treated deliberately.
3. Compare assembled FFT words against an independent one's-complement integer model with these exact truncation points; include both zero encodings, impulses at several indices, all tone bins, near-full-scale noise, and small signals. Check overflow and repeat transforms to detect uncleared imaginary data.
4. Verify power with full MUL low bits and low-limb carry cases. Check DC and Nyquist endpoint weighting and Parseval within a quantization tolerance derived from the accepted FFT error.
5. Check the assembled map, literal pools, symbol-table growth, extended addresses, loader reservations, and return-field/extend-state restoration. Reserve additional code space or a field if the real listing exceeds a budget; never overlap regions silently.
6. Exercise first/last frames of each track, partial final-track padding, checksums, error paths, asynchronous buffer ownership, and field-1 DMA addresses against a disposable image.
7. Measure FFT CPU time, I/O contention, display point cost and refresh continuity with original-speed throttling. Replace estimates with measurements and tune bounded scheduling blocks.

The numeric format, transform sign, evaluation order, scaling gain, endpoint treatment, and sample-track layout are now stable enough to implement. Installation-specific loader/configuration details remain integration checks, not reasons to change the mathematical contract.

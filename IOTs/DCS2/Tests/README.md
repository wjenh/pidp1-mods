# DCS2 Test Suite

Tests for the DCS2 IOT plugin (`IOTs/DCS2/IOT_22.c`), device 22.

## Building

```
make all          # assemble all test programs and build dcstestharness
make harness      # build dcstestharness only
make T01          # assemble T01.am1 only
```

The assembler (`am1`) must be on `$PATH`.

## Running tests

Each test is a PDP-1 program (`.rim` file) plus an optional peer process
(`dcstestharness`).  Start the harness first for tests that need it, then
load and start the PDP-1 program.

The typewriter prints each test step as `<label> pass` or `<label> FAIL`,
and a final summary line (`N PASS, N FAIL, N SKIP`).

The program halts with `IO = testFailCount` (0 = all passed).

---

## T01 -- Client mode basic (port 2101)

**What it tests:** SCBOPEN client, connect wait, ssb, tcb (send), rcs poll,
rch rchclr (receive), SCBCLEAR, channel-closed verification.

**Harness:**
```
./dcstestharness echo-server 2101
```
Start the harness first.  It listens on port 2101, accepts one connection,
echoes all bytes, and exits when the PDP-1 closes.

**Load:** `T01.rim`

**Expected output:**
```
T01-1 connected pass
T01-2 tcb send pass
T01-3 rdy flag pass
T01-4 echo byte A pass
T01-5 echo byte B pass
T01-6 chan closed pass
6 PASS, 0 FAIL, 0 SKIP
```

---

## T02 -- Server mode basic (port 2102)

**What it tests:** SCBOPEN server, wait for accept, ROC, rch rchclr, tcc
(echo), SCBCLEAR.

**Harness:**
```
./dcstestharness send-verify-client 127.0.0.1 2102 41
```
Start harness AFTER the program prints `T02 listening on 2102`.
The harness connects, sends 0x41 ('A'), receives the echo, and exits 0 if
the echo matches.

**Load:** `T02.rim`

**Expected output:**
```
T02 listening on 2102
T02-1 connected pass
T02-2 rdy flag pass
T02-3 recv byte pass
T02-4 tcc echo pass
T02-5 chan closed pass
5 PASS, 0 FAIL, 0 SKIP
```

---

## T03 -- Error paths (no harness)

**What it tests:** dserr flag and specific error codes for: RCH/TCB with no
open channel (dsecc), double SCBOPEN (dseoe), invalid channel number (dseic);
RLE reads and clears last_error.

**Harness:** None required.

**Load:** `T03.rim`

**Expected output:**
```
T03-1 rch no chan pass
T03-2 tcb no chan pass
T03-3 double open pass
T03-4 bad chan num pass
T03-5a rle value pass
T03-5b rle clears pass
6 PASS, 0 FAIL, 0 SKIP
```

---

## T04 -- RRC and ROC (ports 2104, 2144)

**What it tests:** RRC returns cur_chan correctly; ROC overrides cur_chan and
RRC reflects the new value.

**Harness:**
```
./dcstestharness two-echo-servers 2104 2144
```
Start harness BEFORE this program.

**Load:** `T04.rim`

**Expected output:**
```
T04-1 rrc ch0 pass
T04-2 roc->rrc ch1 pass
2 PASS, 0 FAIL, 0 SKIP
```

---

## T05 -- Multi-channel scanner round-robin (ports 2105, 2145)

**What it tests:** With two channels both having data ready, rcr (read +
release) selects them in round-robin order, and each channel receives the
byte that was sent to it.

**Harness:**
```
./dcstestharness two-echo-servers 2105 2145
```
Start harness BEFORE this program.

**Load:** `T05.rim`

**Expected output:**
```
T05-1 both chans pass
T05-2 byte order pass
2 PASS, 0 FAIL, 0 SKIP
```

---

## T06 -- RCS status bit transitions (port 2106)

**What it tests:** dsfopn set after SCBOPEN; dsfcon clear then set after
harness connects; dsfrdy stays clear when harness sends no data; dsfcls set
and dsfcon clear after harness disconnects.

**Harness:**
```
./dcstestharness delay-connect 127.0.0.1 2106 100
```
Start harness AFTER the program prints `T06 listening on 2106`.
The harness waits 100 ms, connects, holds 500 ms, closes.

**Load:** `T06.rim`

**Expected output:**
```
T06 listening on 2106
T06-1 open bits pass
T06-2 dsfcon set pass
T06-3 dsfopn held pass
T06-4 dsfrdy clr pass
T06-5 dsfcls set pass
T06-6 dsfcon clr pass
6 PASS, 0 FAIL, 0 SKIP
```

---

## T07 -- SCBREBIND (port 2107)

**What it tests:** After a client disconnects, SCBREBIND puts the server
channel back into listening state so a second client can connect and complete
a full round trip.

**Harness:**
```
./dcstestharness reconnect-client 127.0.0.1 2107
```
Start harness AFTER the program prints `T07 listening on 2107`.
The harness does two sequential round trips (0x41, then 0x42) with a 500 ms
pause between.  Exit 0 if both echoes match.

**Load:** `T07.rim`

**Expected output:**
```
T07 listening on 2107
T07-1 first conn pass
T07-2 first echo pass
T07-3 first close pass
T07-4 scbrebind pass
T07-5 second conn pass
T07-6 second echo pass
6 PASS, 0 FAIL, 0 SKIP
```

---

## T08 -- RWE wait-for-event (port 2108)

**What it tests:** RWE C blocks the CPU (via ioh) until any socket event
fires on any channel; the connect event from the harness wakes it; dsfcon
is set when the CPU resumes.

**Harness:**
```
./dcstestharness delay-connect 127.0.0.1 2108 300
```
Start harness AFTER the program prints `T08 rwe wait`.
The harness waits 300 ms then connects; this provides the wakeup event.

**Load:** `T08.rim`

**Expected output:**
```
T08 rwe wait
T08-1 dsfcon set pass
T08-2 open+conn pass
T08-3 dsfcls set pass
3 PASS, 0 FAIL, 0 SKIP
```

---

## T09 -- Flexo mode translation (port 2109)

**What it tests:** A channel opened with dcfflex correctly translates
Concise codes to ASCII on send and ASCII to Concise on receive; the special
case where ASCII LF is received and returned as flxnch (013).

**Harness:**
```
./dcstestharness echo-server 2109
```
Start BEFORE this program.  The harness echoes raw ASCII bytes; DCS2 does
the Concise<->ASCII conversion transparently.

**Load:** `T09.rim`

**Expected output:**
```
T09-1 a round trip pass
T09-2 1 round trip pass
T09-3 nl to flxnch pass
3 PASS, 0 FAIL, 0 SKIP
```

---

## T10 -- RXL standalone translate (no harness)

**What it tests:** The RXL IOT performs correct Concise<->ASCII translation
without any channel open, including shift-state change detection and the
"no mapping" return code (flxnch / ascnch).

**Harness:** None required.

**Load:** `T10.rim`

**Expected output:**
```
T10-1 a->flex pass
T10-2 1->flex pass
T10-3 flex->a pass
T10-4 flex->1 pass
T10-5a ucs shift pass
T10-5b after shift pass
T10-6 unmappable pass
T10-7 cunshift pass
8 PASS, 0 FAIL, 0 SKIP
```

---

## T11 -- SBS interrupt on receive (port 2111)

**What it tests:** A channel opened with dcfie + dcfior fires an SBS
interrupt on the configured level when a byte arrives; RIC identifies the
channel; RCS shows dsfior; RCH inside the handler reads the byte correctly.

**Note:** T11 is the most complex test and depends on the PDP-1 SBS
mechanism.  If the SBS infrastructure is not working correctly, T11 will
hang in the ioh wait rather than producing a FAIL.

**Harness:**
```
./dcstestharness interrupt-client 127.0.0.1 2111
```
Start AFTER the program prints `T11 listening on 2111`.
The harness connects, sends 0x41, then waits for the server to close.

**Load:** `T11.rim`

**Expected output:**
```
T11 listening on 2111
T11-1 ric=ch0 pass
T11-2 rcs dsfior pass
T11-3 byte 0x41 pass
3 PASS, 0 FAIL, 0 SKIP
```

---

## Legacy tests

`dcstest.am1` and `dcsecho.c` are the original interactive tests and remain
in this directory.  They are still built by `make` and are useful for manual
interactive verification of the basic client-mode path.

```
./dcsecho           # starts echo server on default port 2022
```
Load `dcstest.rim`, type characters on the typewriter; they are echoed back.

---

## File inventory

| File | Description |
|------|-------------|
| `T01.am1` | Client basic |
| `T02.am1` | Server basic |
| `T03.am1` | Error paths |
| `T04.am1` | RRC / ROC |
| `T05.am1` | Multi-channel scanner |
| `T06.am1` | RCS status bits |
| `T07.am1` | SCBREBIND |
| `T08.am1` | RWE wait-for-event |
| `T09.am1` | Flexo mode |
| `T10.am1` | RXL standalone |
| `T11.am1` | SBS interrupt on receive |
| `dcstestharness.c` | C test peer (all modes) |
| `dcstest.am1` | Legacy interactive client test |
| `dcsecho.c` | Legacy simple echo server |
| `RESUME.md` | Session resume file (for development continuity) |

DCS2 Test Suite -- Session Resume File
=======================================
If a session ends before all files are written, read this file first.
It records what has been planned, what conventions are in use, and which
files exist so the next session can pick up without rework.

## What was approved

A test suite for IOT_22.c (DCS2), targeting 11 test areas T01-T11.
Tests live in IOTs/DCS2/Tests/.  Tests can be resumed by starting the
PDP-1 program and running the matching dcstestharness mode in a terminal.

## Port assignments (hardcoded in am1 and harness)

  T01   port 2101   PDP-1 client,  harness echo-server
  T02   port 2102   PDP-1 server,  harness send-verify-client
  T03   port 2099   PDP-1 server (open only, never connects), no harness
  T04   ports 2104, 2144   PDP-1 two clients, harness two-echo-servers
  T05   ports 2105, 2145   PDP-1 two clients, harness two-echo-servers
  T06   port 2106   PDP-1 server,  harness delay-connect
  T07   port 2107   PDP-1 server,  harness reconnect-client
  T08   port 2108   PDP-1 server,  harness delay-connect (300ms)
  T09   port 2109   PDP-1 client,  harness echo-server
  T10   (no port)   standalone RXL test, no harness
  T11   port 2111   PDP-1 server,  harness interrupt-client

## Test descriptions

  T01  Client mode basic: open client ch0 -> connect -> ssb -> tcb(0x41) -> rch -> verify echo
  T02  Server mode basic: open server ch0 -> wait connect -> rch -> tcc echo -> harness verifies
  T03  Error paths (no net): rch/tcb with no channel (dsecc), double-open (dseoe), bad chan# (dseic), rle reads/clears
  T04  RRC and ROC: open two client channels, verify rrc returns cur_chan, roc overrides it
  T05  Multi-channel scanner: two client channels, harness sends one byte on each, verify round-robin rcr
  T06  RCS status bits: dsfopn after open, dsfcon after connect, dsfrdy after byte arrives, dsfcls after harness closes
  T07  SCBREBIND: server open -> harness connect/send/echo/close -> PDP-1 detects dsfcls -> scbrebind -> harness reconnects -> second round trip
  T08  RWE with completion: server open -> rwe C -> harness connects after 300ms -> PDP-1 wakes -> verify dsfcon
  T09  Flexo mode: client opened with dcfflex, send Concise 061 -> harness receives ASCII 'a' -> harness echoes -> PDP-1 receives Concise 061, LF -> flxnch
  T10  RXL standalone: ascii->flex and flex->ascii spot checks, shift-state change flag, unmappable char returns flxnch
  T11  SBS interrupt: server open with dcfie|dcfior on SBS ch 2 -> harness connects+sends 0x41 -> verify SBS fires, RIC returns ch0, RCS shows dsfior, rch gets 0x41

## Harness modes (dcstestharness <mode> [args])

  echo-server <port>
      Listen, accept one connection, echo all bytes, exit on close.
      Used by T01, T09.

  send-verify-client <host> <port> <byte_hex>
      Connect, send one byte (hex without 0x prefix, e.g. 41), receive
      one byte back, exit 0 if received == sent, exit 1 otherwise.
      Used by T02.

  two-echo-servers <port1> <port2>
      Listen on both ports, accept one connection each (via select),
      echo bytes on both, exit when both close.
      Used by T04, T05.

  delay-connect <host> <port> <delay_ms>
      Sleep delay_ms milliseconds, connect, stay 500ms, close.
      Used by T06 (delay=100), T08 (delay=300).

  reconnect-client <host> <port>
      Connect, send 0x41, receive echo (must be 0x41), close.
      Wait 500ms.
      Reconnect, send 0x42, receive echo (must be 0x42), close.
      Exit 0 both OK, exit 1 on any mismatch.
      Used by T07.

  interrupt-client <host> <port>
      Connect, send 0x41, then wait up to 2s for server to close.
      Exit 0 on clean close, exit 1 on timeout or error.
      Used by T11.

## am1 structure conventions

  - Origin at 100/ (avoids trap/interrupt vectors at 0-7)
  - #include <DCS/dcs2defs.ah>
  - #include <TESTUTIL/reportResult.ac>
  - Entry label: begin
  - Begin with: lio [scbrst]; scb  (triggers init then SCBRESET for clean slate)
  - Report with: lio [REPORT_PASS or REPORT_FAIL]; lac [labelN]; jda reportResult
  - Summarize: jsp printSummary
  - Halt: lio testFailCount; hlt
  - CONTINUE restarts from begin
  - end: constants; start begin

## File status (update this as files are written)

  RESUME.md           DONE
  dcstestharness.c    DONE
  T01.am1             DONE
  T02.am1             DONE
  T03.am1             DONE
  T04.am1             DONE
  T05.am1             DONE
  T06.am1             DONE
  T07.am1             DONE
  T08.am1             DONE
  T09.am1             DONE
  T10.am1             DONE
  T11.am1             DONE
  Makefile            DONE
  README.md           DONE

## Key flexo character mappings used in T09 and T10

  Concise 061 <-> ASCII 'a' (0x61), lower case, no shift required
  Concise 001 <-> ASCII '1' (0x31), digit, no shift required
  ASCII LF (0x0A) received by DCS2 in flex mode -> flxnch (013) returned to PDP-1
  Concise 074 = CSHIFT (upper case shift character)
  Concise 072 = CUNSHIFT (lower case shift character)

  RXL bit layout: IO bit 17 (0400000) = RXL_FLEX (direction: 1=flex->ascii, 0=ascii->flex)
                  IO bit  8 (0000400) = RXL_SHIFTED (shift state on entry)
                  IO bit  9 (0001000) = RXL_CHANGE (shift state changed on this call)
                  IO bits 0-5 = character code

## Error codes reference

  dserr  = 0400000   error flag (bit 0 of 18-bit word)
  dsecc  = 010  (octal)   no current channel (rch/tcb without open channel)
  dseoe  = 002  (octal)   illegal op on already-open channel
  dseic  = 003  (octal)   invalid channel number (>= NUM_CHANS = 8)

## Notes for resuming

  - All am1 programs use octal as default; switch to decimal for port and IP values.
  - The scbrst constant (040000) passed in IO to scb triggers SCBRESET.
  - A server control block is 2 words: (flags | chan_no), port.
  - A client control block is 4 words: (flags | chan_no), port, ip_high, ip_low.
  - IP 127.0.0.1: ip_high=32512 (decimal), ip_low=1 (decimal).
  - For SCBOPEN: IO = scbset | address_of_control_block before executing scb.
  - For SCBCLEAR: IO = 0 | channel_number before executing scb (cmd=0=SCBCLEAR).
  - For SCBREBIND: IO = scbbnd | channel_number (cmd=2=SCBREBIND, scbbnd=020000).

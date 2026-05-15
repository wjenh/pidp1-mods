# Using Dynamic Iots

This document describes the funcionality and now to create your own IOTs.

Updated 15-May-2026

## What is a dynamic IOT?

A dynamic IOT is a compiled shared object that has a specific name and implements specific methods.
When a previously unknown IOT is executed by the pidp-1 emulator, a search is made for a matching dynamic IOT and
if found, the IOT is then handled by the dynamic IOT.
You can add a handler for any IOT that isn't built in by implementing one or two functions, see the sample
`IOT_57.c`.
No changes to the core emulator are needed to add a dynamic IOT, the binding is automatic at runtime.

## What does one look like?

All dynamic IOTS follow a specific naming format and must be in a specific directory.
IOTs are installed in the `opt/pidp1-mods/IOTs` directory and must be named `IOT_nn.so`, where *nn* is the IOT number
**in octal** of the -1's IOT device code.

## How do I install one?

If you have written one in **C**, just put the source \.c file in the above directory and type `make`.<br>
You can see the one required include in any of the examples:
```
#include "iotHandler.h"
```

Alternatively, you can add a directory as was done for the distributed IOTs, just follow the same
pattern.
Make in the IOTs directory will also invoke make in all subdirecories.

## How do I implement one?

At a minimum, implement the `iotHandler(PDP1 *state, int device, int pulse, int completion)` function.
This method is called twice each time the corresponding IOT is executed.

Why twice? This emulates the way the original hardware worked.
Technically, the first occurs at hardware subclock time TP7, the second at TP10.

The first call with the `pulse` argument being 0 mimics the first pulse that would have been sent
to an actual hardware implementation.
The second call with the `pulse` argument being 1 mimics the second pulse that would have been sent.

In the actual system, the first call was generally used to initialize
the specific device or set it up for a command.
The second generally told the hardware to perform an operation.
However, the hardware could do whatever it wanted during the 2 signals.

The first signal came about 2 microseconds into an instruction cycle, the second at about 5 microseconds.

The function *must* return a 1. If it returns 0, that causes the emulator to treat it as an unknown IOT.

It and several other functions are passed a PDP1 \* argument. This is a pointer to the entire state of the emulator.
It can be used to access various registers and even change the pc. But, use with care, don't set random things.\
The include file defines macros for the most common ones:

- AC(pdp1P) - the accumulator
- IO(pdp1P) - the IO register
- MB(pdp1P) - the memory buffer, usually the last word read from memory
- PC(pdp1P) - the program counter
- SENSE(pdp1P) - the sense switches
- SWITCHES(pdp1P) - the test word switches
- PFLAGS(pdp1P) - the program flags
- CKS(pdp1P) - the flags reported by a cks instruction, read only
- SETCKS(pdp1P, flags) - or the flags into those reported by a cks instruction

The entire memory space is available:

- CORE(pdp1)      an array of 64K words, all of memory

This is used as:
```
CORE(pdp1P)[address] = value
value = CORE(pdp1P)[address]
```

The array is of 32 bit integers, only the low 18 bits are used.

There are 3 #defines that can be used:

- MAXBANK       the number of the highest 4K memory bank, 15 decimal
- BANKSIZE      the size of a bank, 409 decimal words
- MAXADDR       the highest valid address, last location in last bank, 65535 decimal

Programs always execute in a bank and cannot access memory ouside the current bank unless extnded memory
operations are done.
The value of the pc is always a 12 bit address within the current bank.
These macros are very useful:

- FULLADDRESS(pdp1P, 12bitaddress)  - converts the local address to the full 16 bit address for CORE()
- CURBANK(pdp1P) - the current bank number, 0-15 decimal
- BANKOF(fullAddress) - the bank number from the full address
- ADDRESSOF(fullAddress) - the bank number from the full address

These are less commonly used:
- pdp1P->sbs16    enable/disable the 16 chan break system
- pdp1P->simtime  the current simulator time in nanoseconds

These can be modified by your code, even the pc.
Adding 1 to the pc will cause the next instruction following your IOT call to be skipped, good for implementing
skip on some condition functionality.
There are a number of other members, but be very careful. You need a good knwolege of the internals of the enulator
to not cause random behavior or total failure.

Note that the panel numbering of the sense switches and the program flags is reversed from the actual bit positions.
That is, flag or switch 6 is the lsb, 01, 1 is 040.

Finally, any IOT in the range of 030 - 037 *automatically* has the IO register cleared before it is called.
This behavior can't be bypassed.
This is again a strange real PDP-1 behavior. So, don't get caught by it if you're passing control information
in the IO register and use an IOT in that range!

## Be Careful!

Your IOT will be running in the pdp1 emulator instance.
If you make a mistake, you can crash it.
The obvious indication of that is that the panel lights stop and switches do nothing.

You can see what the problem was fairly easily, but there are several steps.

First, be sure everything is cleaned up.
This is done most easily by using **pidp1control** to start again, then use it to stop.
This will ensure all the subordinate processes are terminated, like the panel driver and such.

Then, in the /opt/pidp1/src/blinconlnlights/pdp1 directory:\
gdb pdp1\
and type r at the prompt.

But, now how to actually start your program?\
Use *ad1*, the pidp-1 debugger:\
ad1\
and then type:\
load yourprog.rim

When the load finishes, type:\
start

Then, when the crash happens, gdb will tell you where and why.
Most commonly, you forgot to link in a library file in your Makefile
or you trashed memory.

## Starting and stopping

If your IOT implements *iotStart()*, then whenever the pidp-1 enters run state via the start, continue, or read-in
switches, it will be called.
This can be used to do any initialization or reinitialization needed.

If your IOT implements *iotStop()*, then whenever the pidp-1 enters stop state via the halt switch or power off
switch, this will be called.
This can be used to clean up any state or connections you need to deal with.

Both are optional methods.

## Waits, completions, and pulses

Again, IOTs are called twice, once at the internal subclock time TP7 with pulse set to 0, and again
at internal subclock time TP10 with pulse set to 1.
In the original hardware, the first time was intended to have IOT hardware do any setup needed, then the
second time for it to complete its operation.

This isn't normally important to you, **EXCEPT** for the case where an IOT is called in 'wait' mode,
i and c, bits 5 and 6, are 1,0 or 0,1, then you **must** call `IOCOMPLETE()` or IOCOMPLETE_IFNEEDED()
at some point, either in the IOT or in a poll.
Until this is done, the emulator will enter and remain in 'io hold' state.

This mostly applies to 'need completion pulse', where the i,c bits are 0,1.
If the i bit was set, then execution will block on the IOT until a completion is issued.
If the c bit was set, the *ioh* inctruction, *iot i 0* will block until a completion is issued.

Always use IOCOMPLETE() or IOTCOMPLETEIFNEEDED() if your IOT is called with completion set
and process during pulse 1, TP10, unless you have some special requirements.

## Other common functions

If you have any special initialization to do before your IOT is called, implement `iotStart()`.
It will be called whenever the pidp1 goes into run state, or after your IOT is first loaded but
before iotHandler() is called.

If you have any special clenup to do, as the fclose() in the example IOT_57, implement `iotStop()`.
It will be called whenever the pidp1 is halted either by the `hlt` instruction or by the front panel switch.

## Interrupts aka Sequence Breaks

The -1 implements a simple interrupt system that your IOTs can use.
First, read the -1 documentation on the Sequence Break System.
A handler can reqest a sequence break by calling the builtin `initiateBreak(int channel)`.
This is typically called when pulse is 1, but can be called asyncrhonously from an *iotPoll()*.

If the 16 channel break system is installed, the channel numbers are 0-15.
The channel in question must have been enabled via the enable sequence channel IOT `asc`, which has the format
`72nn51` where *nn* is the channel number 0-15 decimal, 0-17 octal,
Similarly, a channel can be disabled with the disable sequence break IOT `dsc`, 72nn50.

If the standard one channel sbs is installed, any channel number is ignored and will always be treated as 0.
IOTs asc, dsc are also ignored in this case.

For either system, sequence break must be enabled in general via the enter system break mode IOT, `esb`, 72xx55.
It can be disabled via the leave system break moode IOT, `lsb`, 72xx54.

## Special functions
If the IOT being inmplemented should be processed by another IOT, just implement the `iotAlias()`
function, which should return the IOT number of the handler to actually process it. Remember to keep your octal
vs decimal numbers correct.
That IOT will be loaded if it hasn't already been.
No other functions need to be implemented, they will be ignored.
Whenver your alias IOT is executed, the alias target code will actually be invoked.
You can tell what IOT device code caused the invocation by looking at the `dev` parameter.

The code for the Type 23 Parallel Drum gives examples of this.

If your code needs to be periodially activated, implement the `iotPoll(pdp1P)` function.
If polling is enabled in your IOT via `enablePolling(when)`, then the emulator will call
iotPoll() once every specified number of instruction cycles between the end of subclock TP10 and the start of TP0.
A `when` of 0 disables polling for your IOT.

Until that is done, you will continue to be polled every `when` instruction cycles.
One cycle is 5 microseconds, so the minimum granularity is that.
If you don't need to be polled as frequently, set a longer poll interval to reduce processor loading.

See many of the proviedd IOTs for examples.

It's good practice to disable polling if your IOT has finished what it needs to do, you can enable it again
when necessary.

## Library code

Some useful library functions are provided in the Lib directory, see them for details.
Examples are also in DCS2, Type62and64Printer, etc.

## Logging

A logging facility is provided:
```
#define DOLOGGING
#include Logger/iotLogger.h
```
within your code:
```
iotLog(boolean enable, "format like printf", ....);
```

You can use `iotCloseLog()` to close the log file but it's not actually required.
The output to the log file is flushed after each log call.
The debug output will be written to the file `/tmp/iot.dbg`.

You can log different kinds of events, controlled by the `enable` flag. If it is 0, nothing will be logged,
nonzero, the message will be logged. See some of the provided IOTs for examples.

If DOLOGGING is not defined then any log statements are dropped.
This allows debugging to be turned on and off.

## Configuration for your IOTs

If you want to have configurable features controlled via the /opt/pidp1-mods/pidp-1.config file, it's easy to do.
Two functions are provided automatically, you don't have to explicitly link them:

```
ConfigurationP getConfiguration(void)
ConfigurationSettingP findConfigurationSetting(ConfigurationP configP, char *nameP)
```

See the code in the Type62and64 directory for examples.

## All functions and defines

- int iotHandler(PDP1 \*hardwareP, int device, int pulse, int completion)
- void iotStart(void)
- void iotStop(void)
- void enablePolling(int cycles)
- void iotPoll(PDP1 \*hardwareP)
- void initiateBreak(int chan)
- int iotIsAlias(void)

Defines

- IONOWAIT(PDP1 \*hardwareP)  tell emulator to ignore the wait bits in the IOT instruction
- IOCOMPLETE(PDP1 \*hardwareP) tell the emulator the wait state is ended
- IOCOMPLETE_IFNEEDED(PDP1 \*hardwareP, int complete) tell the emulator the wait state is ended if complete is not 0

## Final notes

If you use the **am1** assembler, include files for the provided IOTs are in the /opt/pidp1-mods/Am1 directory.

IMPORTANT - once loaded, a handler stays loaded until the pidp1 emulator is shut down and restarted.
So, if you change your handler, restart or it won't work properly.

Finally, remember that this is just an emulation of the hardware. There are no acutal electrical start or continue
pulses going to anything. So,the emulator will be blocked until your `iotHandler()` function returns.
If it takes too long, the following emulator cycle will be delayed.
This will always happen if an IOT instruction is called with a wait condition on until a completion is issued.

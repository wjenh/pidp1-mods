# Using the DCS Communication System

This document describes how to use the enhanced multi-channel Type 630 Data Communication System replacement.
Updated 14-Sep-2026\
add new telnet rules

## What is DCS2?

The original DCS was a multichannel serial I/O system for the PDP-1, -4, -5 and -6. It supported from 8 to 64
serial connections to, e.g. teletypes or modems.
There were multiple variations covering the number of channels, the transmission speed, full or half duplex, etc.
However, this really isn't particularly useful in today's world.

This is a modernized version of DCS which uses sockets instead of serial connections
to allow communicating with any device that has sockets, or any device that can open a remote socket.
It is full duplex wih separate send and receive buffers and control and can be configured at compile time
for up to 64 independent channels.

The instructions are the same codes as the original and with very similar function, although
with extensions to manage the sockets associated with each channel.

As with the original, DCS2 provides only single-character-at-a-time operations, similar to the standard typewriter
interfaces, tyi and tyo.

The original DCS allowed or-ing 02000 with the IOT to cause the IO register to be cleared before new data was added.
and the behavior is maintained for rch and rcr.
This allows multi-character words to be efficiently handled, as with the macro assembler **flexo** directive.
Only the low 6 bits for flexo mode or 8 bits for ascii mode are used for the commands.

For all others the IO register is cleared if data is going to be returned in it.

DCS2 is implemented as a dyamic IOT, IOT_22 and requires no changes to the emulator as long as it has
the Dynamic IOT extension. Just copy IOT_22.so to the IOTs directory, that's it, not even a restart is needed.

## How does it work?

In order to use one of the 'channels', an IOT must be used to tell DCS2 how to operate, such as
server expecting a connection on the local socket, client opening a connection on a remote socket.
The configuation also specifies the DCS2 channel to assign and other information.
The IOT passes the address of a configuration block in local memory.
Bytes can then be sent and received via the IOT commands below.

The original and this version implement a 'scanner' that looks at incoming connections to find one that
has input ready to read.
A single channel is assigned as the current channel and it maintains control until it releases control,
which then allows the scanner to select a new channel.
It does this in a round-robin fasion so one channel can't monopolize control.
If the current channel is locked, then the ready channel is just marked as ready to read and becomes
a candidate for selection.

For sending, either the current channel can be used or a different channel can be specified.
This does not affect the scanning algorithm, an explicit send channel selection remains valid until
changed.

An explicit channel selection will automatically unlock the current channel and lock the new one.
Again, it must be released before scanning will continue from the next channel after.

Internally the IOT uses epoll() to maximize performance and reduce overhead.
No channel will be selected by the 'scanner' unless it has data to read.
It is assumed that data can always be sent, but error are possible and can be detected.

## Client vs server mode

Each channel can be either a TCP/IP client or server.

For server mode, the channel listens on the specified port for a connection request.
A check is made for a request every 100us.
When a connection is established, the status of the channel will indicate so.
Only one remote system can connect to a channel and a channel will not be selected by the scanner
until a channel is connected.

Multiple channels can have the same port in which case they act as a connection pool.
When a request comes in, the first channel in listening state with the requested port will be assigned to that client.

A channel can be closed to terminate the connection, or the connection itself can be closed by the client.
If the connection is closed, a server channel will return to listening for a connection.
A client channel will be close and can only be used by opening it again.

For client mode, when the channel is opened, it tries to connect to the specified host.
In order to not halt the system while waiting, a non-blocking connect is used.
As with server mode, it will be polled every 100us.
When it is established, the channel becomes a candidate for selection by the scanner and can be used
by the send commands.

## Flexo, FIODEC, Consise?

There are two character sets, Concise and FIODEC.
They are virually identical except FIODEC has bit 0200 set as a parity bit.
It is set to ensure an odd number of 1 bits in the character.

To convert from/to Concise to/from FIODEC:

```
fio = (con & 1)?con:(con | 0200);
con = fio & 077;
```

So, what is 'flexo'?
It's just Concise code, that's it.
This is what the macro directives char, flexo, and text produce.

During ascii to flexo conversion, some characters cannot be represented.
In this case, the special character flxnch, 013, will be returned.
The full list of special characters is:

- flxnch 013    no flexo equivalent for ascii character
- flxetx 013    end marker for some string operations
- flxerr 076    error marker, some error happened in rcr or rch
- flxnl  077    the end-of-line character, flexo carriage return
- ascnch 077    a flexo shift or unshift character was seen by rxl in flexo to ascii mode

## Carriage return, linefeed, newline, echo?

This is an area on which there is plenty of variation. Some systems indicate end-of-line with a linefeed
character, aka newline, ascii 012. Some use carriage return, 015, followed by linefeed. Some just use carriage return.
DCS2 has configurable behavior, but the behavior depends upon the flexo mode.

If echo mode is on, each incoming character is echoed back to the remote host as it was received,
before any Flexo or cr/lf conversion, at the moment the program reads it with rch or rcr.
If it's off, then no echoing is done.
See the rules under the IOT commands for the details.

If flexo mode is on, the rules are simple.
Flexo does not have a linefeed character, only carriage return, 077.
If telnet cr/lf  is not on, this will be converted to lf, the newline character, on output.

A carriage return on input will be converted to flexo 077. A newline will be ignored.
If telnet cr/lf is on, then sending a flexo carriage return will result in sending an ascii
carriage return followed by an ascii newline.

If flexo mode is off, then the rules are a bit different.
If telnet cr/lf is on, then sending an ascii newline will result in a carriage return followed by a newline
being sent, and sending a carriage return will result in a carriage return followed by a nul, the telnet standard's
form of a carriage return on its own.
If telnet cr/lf is off, both are sent as they are.

See the section on Telnet cr/lf procesing for more detail.

## Notes on waits, completion pulses, and channel starvation

Only one command, *rwe*, below, will honor either the wait bit (i) or the completion pulse request.
All others will always immediately return.

Why? Remember that this is an asynchronous socket communications layer. Blocking in a wait could mean that other
active channels you have opened would not get serviced until the current wait completed.
This is not good for the socket world, you could lose data.

So why does *rwe* wait? Because it isn't waiting on one channel, it responds to any event on any channel and so
can't result in a service blocking wait.

If you want to not have to check busy flags constantly, you should use *rwe* or interrupts to know when some
socket event has occurred.

It's also a very bad idea to pay attention to just one channel, assuming you have multiple channels open.
The primary commands use the current channel and must unlock it for another channel to be automatically selected.
You can also use the *roc* command, below, to override the current channel and
force a new channel to be the active, locked one.

## Errors

For most commands, the IO register will contain an error status on return.
If bit 0 is 1, an error occured.
If bit 1 or bit 2 is also 1, it is one of the two conditions a program expects to see,
no character ready or the transmit buffer full, and the rest of the word is not an error code.
Otherwise the DCS2 error code is in bits 14-17,
and if bit 3 is also 1, the low 8 bits of the Linux errno are included in bits 6-13.
See the section on the error word, below, for the full layout and the DCS2 error codes.

## The IOT commands

**Note**: The current memory bank is honored in all calls that are passed 12-bit addresses, so DCS2 can be used
in extended memory mode.

**Warning**: The mnemonics below are directly from the DEC documentation, but notice there is an *rcr* instruction.
That conflicts with the rcr that is rotate-combined-right.
Keep that in mind. The include files provided use rchr instead.

An am1 #include file is provided, #include <DCS/dcs2defs.ah> that contains mnenomics for all the commands
and the bit flags, as used here.

All are implemented via a single IOT, 22 as mentioned.
The following IOT commands are supported:

- rch 720022 receive a character from the current receive channel into the IO register
- rcr 721022 same as rch except also release the channel, combines rch and rsc
- tcb 724022 send a character from the IO register to the current send channel
- tcc 725022 send a character fron the IO register to the current receive channel
    and release the channel
- rrc 720122 get the active channel number into IO register bits 12-17
- rsc 721122 release the current receive channel
- ssb 724122 select a new send channel, IO register bits 12-17 select the channel

Note again that setting bit 02000 on rch and rcr will clear the IO register before reading the next
character.
All other commands except rwe always change the IO register on return.

No operations will succeed until at least one channel has been opened via scb, below.

Flexo mode is honored for rch, rcr, tcb, and tcc.
The characters will be converted to ascii before sending, or converted from ascii on reading.
See the detailed description in the Channel Control Block description below.

Rcr, rch, tcb, and tcc use the low 6 bits in flexo mode, else the low 8 bits.

For rch and rcr, the action on an error depends upon if flexo mode is enabled.
For flexo, for no character ready the low 6 bits returned will be flxnch.
For any error, the low 6 bits returned will be flxerr.

For 8-bit mode, if no character is available IO register bits 0 and 1 will both be 1, bits 10-17 will be 0,
and the other bits unchanged.
If the connection was lost, bit 0 will be set, bit 1 cleared, and the *connection was lost* error code
added to the register.
Using rchclr with rch and rcr makes both of these the whole error word.

The tcb and tcc commands will fail if the channel isn't open and connected,
the transmit buffer is full, or there is a socket error.
A socket error is most likely because the remote end isn't listening any more.

if bits 0 and 2 are set on return, the send buffer is full, the other bits will be left unchanged.
The character was not sent.
When space is again available, the channel status will be updated.
If any other error occurs, bit 0 will be set, bits 1 and 2 cleared, and the rest of the register set to the full
error code.

If no errors occurred for tcb and tcc, the IO register will be cleared.

If Flexo is enabled, upper and lower shift characters do not get sent, they are just state changes.
Don't count them as sent characters.
Any Concise character that has no ascii mapping will be ignored.

The most recent error from any command, or found by DCS2 on any channel while polling,
is saved and can be fetched via the rle command.
It is not per channel, and a successful operation does not clear it, only rle does.

If echo is enabled, each character is sent back exactly as it was received,
at the moment the program reads it with rch or rcr.
Nothing is echoed for a character the program has not read yet, and nothing is echoed
for the telnet protocol bytes DCS2 handles itself.
In telnet mode, a cr/lf pair that is read as one character is echoed as the pair,
a cr/nul pair is echoed as cr/lf, the line end it stands for,
a cr read with no lf after it is echoed as a cr followed by a nul,
and a received 0377 is echoed as the telnet escape pair 0377 0377.
The echo setting in effect when the character is read is the one that applies.

For rrc, if there is no active channel, that is, no channel is being held,
on return IO register bit 0 will be a 1 and the NOCHANNEL eror will be set.

And the new extended commands:

- scb 724222 set/clear/rebind/modify channel request\
    On call:
    ```
    IO register bit 5 set to 1 is a set request
    IO register bits 6-17 are the address of the request block in memory
    ```

    ```
    IO register bit 4 set to 1 is a rebind request
    IO register bits 12-17 are the channel number to rebind
    ```
    Rebind only applies to server channels.\
    This closes any currently open socket on the channel and returns to listen mode.\
    Anything the closed caller sent that the program has not read is discarded, the next caller starts afresh.

    ```
    IO register bit 3 set to 1 is a reset request, no other bits used
    ```
    A complete reset of DCS2 is done.\
    A reset will close all open channels as well as clear internal state and disable polling,
    resetting DCS2 to its startup state.

    ```
    IO register bit 2 set to 1 is a modify request
    IO register bits 6-17 are the address of the modify request block in memory.
    ```
    The channel named in the modify request block has some of its operating parameters changed.
    No other channel is affected.\
    A modify can be issued at any time while a channel is open, connected or not.\
    If the channel is not open, the *not open* error is returned and nothing is changed.\
    See the section on the Modification Request Block for details.

    If neither set, reset, modify, or rebind is set, this is a clear request.
    ```
    IO register bits 12-17 are the channel number to clear
    ```
    The channel's ports will be closed, it will have its open flag cleared, and it will be avaialble
    for setting again.

    A channel set as a server will listen on the assigned port for a connection request.
    When a connection is established, the channel status will indicate so.

    A channel set as a client will attempt to open the socket and port specified in the request block.
    When the connect succeeds, the channel status will indicate so.

    A clear on either type of channel will close any open connection and mark the channel as not in use.
    
    On a reset, all open channels will be closed, polling stopped, and the DCS2 system
    reset to inactive. A subsequect channel set request will reinitialize it.
    A reset has no request block, any address is ignored.

    Setting more than one of the request bits is an error and will set the *invalid command*
    error code.

    On return from any of the above, the IO register will be cleared for success.
    On an error, bit 0 will be set and the error code returned.

- rle 724322 get last error
    ```
    On return the IO register will contain the full error word of the last error seen
    ```
    This is useful for getting error codes that aren't returned as part of a command return.

- rpc 724422 receive pending count
    ```
    On call, IO register bits 12-17 should contain the channel number to check.
    On return the IO register will contain the number of bytes currently waiting to be read.
    ```

- rci 724522 clear and reset interrupt status
    ```
    On call, IO register bits 12-17 should contain the channel number to use.
    On return, the IO register will be unchanged unless the channel number is invalid
    in which case bit 0 will be 1 and a channel out of bounds error set.
    ```
    The channel's interrupt-in-progress state is always cleared, whether or not interrupts
    are currently enabled for it.
    If interrupts are enabled for the channel, by the channel request block or by a later modify,
    this also reenables them.

    An event that happens on the channel while its interrupt is being handled is held, not lost.
    Once rci has been done, the held events are requested as one new interrupt,
    and rcs will show all of their causes.
    rci should be the last thing an interrupt routine does for its channel.
    Doing it earlier allows that next interrupt to be requested while the routine is still running.

- ric 724622 get interrupting channel
    ```
    On return, the IO register will contain the last channel number that caused an interrupt
    or 0100, an invalid channel, if none.
    ```

- rcs 724722 get the status for a channel
    ```
    On call, IO register bits 12-17 should contain the channel number to use.
    On return, the IO register will contain the the full status for the channel.
    ```
    See the section on channel status for details.

    If an invalid channel number is passed, the invalid channel status will be returned.

- rwe i 735122 wait for event
    ```
    There is no input or output, the IO register will be unchanged.
    ```
    This is the only command that will block, and it will always block.
    Why? Because any event on any channel will release the wait and so
    avoid starving channels, no wait can occur on a single channel.

    It will resume if a connection is established, a channel has a character ready to read,
    a previously-full transmit buffer becomes available to write, the remote end closed a connection,
    or for any socket error.

    If the wait bit is not set, op code 725122, it will return immediately.

- roc 725222 override current channel
    ```
    On call, IO register bits 12-17 should contain the channel number to use.
    On return, the IO register will be 0 unless:
    If an invalid channel number is passed, the invalid channel status will be returned.
    If the channel requested is not open, the not open status will be returned.
    ```
    The new channel must be open.
    The current channel, if any, is unlocked.
    The new channel becomes the current channel and will be locked.
    If an error occurred, the current channel and lock state will be unchanged.

- res 725322 enable/disable sbs16
    ```
    On call, IO register bit 17 set to 1 enables sbs16, else disables it.
    On return, the IO register will have the prior setting.
    ```

- rxl 725422 convert between flexo and ascii
    ```
    On call:
    IO register bit 0 set to 1 is flex to ascii, otherwise ascii to flex
    IO register bit 9 set to 1 means upper-shift for flex, see note
    IO register bits 10-17 are the character to convert
    On return:
    IO register bit 0 will be unchanged
    IO register bit 8 set to 1 for ascii to flex means the flex shift state changed
    IO register bit 9 set to 1 for ascii to flex means in upper shift state
    IO register bits 10-17 are the converted character
    ```
    This is a convenience IOT that will convert back and forth between flexo/concise and ascii.
    It uses the same mappings as the regular send and receive translations.

    The ineraction of bits 8 and 9 are important.

    Bit 9 is the current shift state, set to a 1 if currently in upper shift mode, else 0.
    Typically, this would initially be set to 0.

    **IMPORTANT**, after the initial setting, the application should not change this bit until the current
    character stream is competed. Otherwise, a valid character might never be returned.
    When a shift, upper or lower, occurs, bit 9 will be updated to reflect the shift state
    and bit 8 set to indicate a shift change happened.

    For ascii to flex conversion, the character returned will be the appropriate shift character.
    If there is no equivalent flex character for an ascii character, flxnch will be returned.

    For flexo to ascii conversion, a shift character will be ignored and the returned character will be ascnch.

    For either mode, the application should resubmit the character if bit 9 is a 1.
    It can also check for ascnch in flexo to ascii mode or an upper or lower shift character in ascii to flexo mode.

## The Channel Request Block

The request block varies depending upon whether the channel is a server or a client.
A set for a server requires 2 words, a client 4 words.
A clear, rebind, or reset use no words, just the IO register contents.
A modify uses the modification request block, described in the next section.

Flags in word 0 of the request block for a set:

nfECerissssmcccccc

where:
```
bit 0, n        use telnet protocol
                See below for details.
bit 1, f        do Flexo conversion
                Outgoing characters are converted to ascii from Flexo,
                the reverse for incoming ones if there is a mapping,
                0 to just pass the data unchanged 8 bit binary unless
                telnet cr/lf mode is on.
bit 2, E        echo input
bit 3, C        interrupt on connecton established or lost
bit 4, e        interrupt on a socket error other than established or lost
bit 5, r        interrupt on recieved characters available,
                or when a full transmit buffer has room again
bit 6, i        1 to allow interrupts
bits 7-10, ssss if i is 1, the SBS channel to use to interrupt,
                always treated as 0 if no SBS16
bit 11, m       1 for server, 0 for client
bits 12-17, cccccc the channel number up to 63 depending upon configuration
```

Enabling interrupts here immediately makes the channel a candidate for interrupts,
so be sure you have set up your interrupt locations, etc. that the sbs interrupt system
will use, and then enable it via the esm IOT.

For a server channel:

Word 1 bits 2-17 specify the port number to listen on

For a client channel:

Word 1 bits 2-17 specify the port number to connect to

Word 2 bits 2-17 specify the high 16 bits of the IP address to connect to

Word 3 bits 2-17 specify the low 16 bits of the IP address to connect to

Thus, an ip address of 10.20.30.40 would have word 2 bits 2-8 set to 10, 9-17 set to 20,
word 3 bits 2-8 set to 30, word2 bits 9-17 set to 40.
Remember, these are decimal values, the octal equivalents would be 012, 024, 036, 050.

Once a channel has been opened, the request block is no longer used and may be reused for other purposes.
A channel can only be bound to one port at a time.
For server mode, allocate multiple channels with the same port to allow multiple connections to that port.
If there is no channel available to accept a connection request, the request will be processed when a channel
becomes available for the associated port.
Note that the client might abandon the request after some time limit.

## The Modification Request Block

The modification request block updates some characteristics of one open channel.
It is at least one word and may be additonal words depending upon its request settings.
As with the channel request block, it is no longer used once the scb returns and may be reused.

Flags in word 0 of the request block:

ttmmmmmmmmmmcccccc

where:
```
bits 0-1, tt         the type of modification
                     See below for details.
bit 2-11, mmmmmmmmmm modification type specific flags
                     See below for details.
bits 12-17, cccccc   the channel number up to 63 depending upon configuration
```

The modification type is one of:
```
0  general modifications
1  telnet modifications, see below
2-3 unused
```
A modification type of 2 or 3 is an error and will set the *invalid command* error code.
Until the telnet modifications are specified, type 1 is also an *invalid command*.

A modification applies only to the channel named in bits 12-17.
The channel may be a server or a client channel.

### Modification type 0, general modifications

Word 0 of a type 0 request uses the same bit positions as word 0 of the channel request block,
with the bits that cannot be modified set to 0:

00ECerissss0cccccc

where:
```
bits 0-1, 00       the modification type, 0
bit 2, E           echo input, 1 to enable, 0 to disable
bit 3, C           interrupt on connecton established or lost
bit 4, e           interrupt on a socket error other than established or lost
bit 5, r           interrupt on recieved characters available,
                   or when a full transmit buffer has room again
bit 6, i           1 to allow interrupts
bits 7-10, ssss    if i is 1, the SBS channel to use to interrupt,
                   always treated as 0 if no SBS16
bit 11, 0          not used, ignored
bits 12-17, cccccc the channel number
```
Because the positions match, the same include file symbols are used, e.g. *dcfecho*, *dcfie*, and *dcmsbs*.

Telnet mode, Flexo mode, and server/client mode cannot be changed by a modify.
**Be careful when copying a channel request word:** bits 0 and 1 are the telnet and Flexo flags there
but are the modification type here.
A word with *dcfflex* set is a type 1 (telnet) request, and one with *dcftel* set is type 2 or 3,
which is an *invalid command*.

A type 0 request is one word.
The flags given **replace** the channel's current settings.
Every one of E, C, e, r, i, and ssss takes the value given, so a flag left 0 is turned off.
There is no way to read the current settings back, so the program should keep its own copy
of the word it last used.

The effects of a change:
```
Echo            Takes effect for the next character the program reads,
                including one that arrived before the modify.
Enabling        Interrupts are enabled for the channel.
interrupts      If a character is already waiting to be read and r is 1,
                the interrupt is requested immediately, it does not wait for the next character.
                Connection and error events that happened while interrupts were disabled
                are not replayed, use rcs to see the channel's state.
Disabling       Interrupts are disabled for the channel, and any interrupt in progress
interrupts      or pending for it is discarded, as if rci had been done.
Changing ssss   Takes effect for the next interrupt requested.
                An interrupt already requested is not moved.
```

## Telnet mode rules

Enabling telnet is transparent to the pdp-1 program side.
It handles the telnet protocol itself and transforms cr and lf characters internally.

DCS2 offers to echo and to suppress go-ahead, and accepts a client's agreement,
whether the client negotiates first or waits for the server; every other option is refused.\
The offer goes to every new connection on a server channel, and asks a real telnet client for character mode.
A client channel makes no offer and refuses every option.\
Whether a character is echoed stays the program's choice (dcfecho and the modify request),
even when a client asks DCS2 not to echo.

Each connection starts afresh: nothing a previous caller sent reaches the next one,
whether the server channel took the new caller by itself or after a rebind.

On output, Flexo carriage return is converted to cr/lf.\
Ascii lf, aka newline, is converted to cr/lf on output.\
Ascii carriage return is sent as cr followed by a nul, as the telnet standard requires for a carriage return
with no lf after it.
DCS2 cannot know what the program sends next, so a cr then an lf go out as cr, nul, cr, lf,
which a terminal shows the same as cr/lf.

On input, in Flexo mode cr/lf is converted to Flexo carriage return.\
A linefeed with no carriage return is also converted to Flexo carriage return, not strictly conforming.\
In ascii mode cr/lf is converted to lf, a newline.\
A bare lf is left unchanged.\
A cr followed by a nul is taken exactly as cr/lf, in both modes, as the telnet standard requires of a server;
it is what a client sends for a carriage return on its own, and some send it for the Return key.
The nul never reaches the program.
A nul anywhere else is data.

This is close to the telnet standard, but sightly relaxed.

Telnet setings can be modified by using a modification request block with a modification type of 1.
Until they are specified, a type 1 request returns the *invalid command* error.
The supported modifications are:

??????????????????

where:
```
To be specified
```

## Channel status bits

If a channel's status is fetched via rcs, the status bits are:

```
Bit  0  unused
Bit  1  unused
Bit  2  unused
Bit  3  unused
Bit  4  unused
Bit  5  unused
Bit  6  is the current channel
Bit  7  interrupted because a connection was established or lost
Bit  8  interrupted because of a socket error or the transmit buffer is full
Bit  9  interrupted because a character was received or a full transmit buffer has room again
Bit 10  interrupts enabled
Bit 11  remote closed connection
Bit 12  has a socket error
Bit 13  has pending characters to read
Bit 14  send buffer is full and can't take more characters
Bit 15  is connected and ready for use
Bit 16  is a server
Bit 17  is open
```

When an interrupt occurs, bits 7-9 can be used to determine why.
More than one can be set if events were held while an earlier interrupt was being handled.
For connects or disconnects, bits 15 and 11 will indicate the state.
For bit 9, bit 13 shows whether a character is waiting, and bit 14 clear shows the transmit buffer has room.

## Check Status, cks, bits

If check status is executed, IO register bit 16 will be 1 if a channel is locked.

## The error word

This error word can be returned in the IO register and is also the last error reported by rle.
See the individual commands for more details.

```
Bit  0      1 means an error occurred
Bit  1      no character was ready, rch and rcr in 8-bit mode only
Bit  2      the transmit buffer is full, tcb and tcc only
Bit  3      1 means the Linux errno is included in bits 6-13
Bits 4-5    always 0
Bits 6-13   the low 8 bits of the Linux errno, if bit 3 is 1, else 0
Bits 14-17  the DCS2 error code
```
Bits 1 and 2 are conditions rather than failures.
When either is set, bits 3-17 of the IO register do not hold an error code,
see rch, rcr, tcb, and tcc for what they do hold.
The no-character condition is not recorded for rle.
A full transmit buffer is, and rle returns it with bits 3-17 0.

The DCS2 error codes are in bits 14-17:
```
01 - not open, a request is being made on a channel that is not currently open
02 - already open, an scb set was done on an already open channel
03 - illegal channel, a number greater than the number of allowed channels has been used
04 - not server, a scb rebind command is being made on a channel that is not an open server channel
05 - invalid command, an scb with more than one request bit set, or a modify with an invalid type
06 - socket error, an open or other operation on a socket failed
07 - bind error, a bind() on a port failed
10 - no current channel, no channel is current and a current-channel comnmand was issued
11 - connection was lost, the socket is no longer connected, probably a client close
12 - epoll error during polling
13 - no remote connection established but a write, tcc or tcb, was done
```
These are the octal values.

## Interrupts aka Sequence Breaks

The DCS2 system can use the SBS or SBS16 system by specifying such in the request block.
If only SBS is available, the channel number is ignored and is always 0.
On an interrupt, ric can be used to see which channel, if any, needs attention.

An interrupt can occur if any error condtion is raised, a connection is established or lost,
if any bytes are ready to be read, a transmit buffer is full, or a full transmit buffer has room again,
controlled by the settings
specified when the channel was opened or by a later modify request.

If the SBS16 system is in use, different SBS channels can be assigned to each DCS2 channel for convenience.

## What the include file provides

NOTE that rcr in DCS conflicts with rcr, rotate-combined-right.
It is renamed rchr here.

These are included:
```
#define rch iot 0022
#define rrc iot 0122
#define rchr iot 1022
#define rsc iot 1122
#define tcb iot 4022
#define ssb iot 4122
#define tcc iot 5022

// rch and rcr will clear the IO register if
// this is added to the IOT, else it ors.
#define rchclr 002000

// Extended commands.
#define scb iot 4222
#define rle iot 4322
#define rpc iot 4422
#define rci iot 4522
#define ric iot 4622
#define rcs iot 4722
#define rwe iot 5122
#define roc iot 5222
#define res iot 5322
#define rxl iot 5422

// dcfxxx are single bit flags
// dcmxxx are multibit masks
// Channel Request Block first word
// dcftel - enable telnet mode: symmetric cr/lf framing on send and receive plus
//          limited IAC handling (a server offers ECHO and SGA, other options
//          refused, IAC escaping honored).
// dcfflex - flexo mode, do automatic conversion
// dcfecho - echo received characters
// dcfioc - interrupt on connection open or close
// dcfioe - interrupt on socket error
// dcfior - interroupt on character received
// dcfie -  enable interrupts for channel
// dcfsrv - server mode
// rxlfta - flexo to ascii
// rxlucs - flexo upper case shift
// rxlchg - flexo shift state changed
// dcmsbs - bitmask, sbs channel to use
// dcmcha - bitmask, channel number for this channel

#define dcftel 400000
#define dcfflex 200000
#define dcfecho 100000
#define dcfioc 040000
#define dcfioe 020000
#define dcfior 010000
#define dcfie 004000
#define dcfsrv 000100
#define rxlfta 0400000
#define rxlucs 00400
#define rxlchg 01000

#define dcmsbs 003600
#define dcmcha 000077

/// Directives to scb
#define scbset 010000
#define scbbnd 020000
#define scbrst 040000
#define scbmod 100000     // TENTATIVE name, 13-Sep-2026
#define scbclr 000000

// TENTATIVE names, 13-Sep-2026: this block and the error-word block below
// are not final and may be renamed before the release.
// Modification Request Block first word
// dcmmty - bitmask, the modification type
// dcmgen - modification type 0, general
// dcmtel - modification type 1, telnet
// Type 0 uses dcfecho, dcfioc, dcfioe, dcfior, dcfie, dcmsbs, and dcmcha from above.
#define dcmmty 600000
#define dcmgen 000000
#define dcmtel 200000

// Channel status flags
// dsfopn - channel has been opened by scb
// dsfsvr - channel is a server channel, else client
// dsfcon - connected to remote host
// dsfful - socket transmit buffer is full
// dsfrdy - has characters ready to read
// dsfser - got a socket error of some kine
// dsfcls - remote closed connection
// dsfien - interrupts are enabled
// dsfior - interrupted because a character was received
// dsfioc - interrupted because a connection was opened or closed
// dsfioe - interrupted because transmit buffer full or other socket error
// dsfcur - is the current channel

#define dsfopn 000001
#define dsfsvr 000002
#define dsfcon 000004
#define dsfful 000010
#define dsfrdy 000020
#define dsfser 000040
#define dsfcls 000100
#define dsfien 000200
#define dsfior 000400
#define dsfioc 002000
#define dsfioe 001000
#define dsfcur 004000

// and some combined ones
#define dssrdy dsfopn+dsfcon
#define dsserr dsfcls+dsfser+dsfful

// Errors returned in IO, dserr is the error flag
// dseno - channel not open
// dseoe - illegal operation on open channel
// dseic - invalid channel number
// dsens - server command on non-server channel
// dseil - illegal command, commdand with illegal options
// dseso - socket open or operation failed
// dsebi - server bind to port failed
// dsecc - rch, rcr, tcb, or tcc done but no current receive or send channel
// dselo - remote end closed connection
// dseep - internal error in epoll
// dsenc - no client connected but operation attempted
#define dserr 400000

// The rest of the error word, see the section on the error word
// TENTATIVE names, 13-Sep-2026
// dseflc - flag, no character was ready (rch, rcr, 8-bit mode)
// dseflf - flag, the transmit buffer is full (tcb, tcc)
// dsefle - flag, a Linux errno is in dsemen
// dsemen - bitmask, the low 8 bits of the Linux errno
// dsemco - bitmask, the DCS2 error code, one of the dsexx values below
#define dseflc 200000
#define dseflf 100000
#define dsefle 040000
#define dsemen 007760
#define dsemco 000017

#define dseno 1
#define dseoe 2
#define dseic 3
#define dsens 4
#define dseil 5
#define dseso 6
#define dsebi 7
#define dsecc 10
#define dselo 11
#define dseep 12
#define dsenc 13

// check status bit in IO reg
#define dcfcks 000200

// special flex characters
#define flxerr 076
#define flxnch 013
#define flxetx 013
#define flxnl 077
// ascnch is used by rxl
#define ascnch 077
```

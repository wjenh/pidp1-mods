/*
 * Telnet emulation used by various components.
 *
 * 14-Jul-2026 wje first major cleanup pass, it needed it.
 * 7-Oct-2026 wje fix a number of errors in the original around IAC, etc.
*/
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>

enum {
    SE = 240,
    NOP = 241,
    BRK = 243,
    IP = 244,
    AO = 245,
    AYT = 246,
    EC = 247,
    EL = 248,
    GA = 249,
    SB = 250,
    WILL = 251,
    WONT = 252,
    DO = 253,
    DONT = 254,
    IAC = 255,

    XMITBIN = 0,
    ECHO_ = 1,
    SUPRGA = 3,
    LINEEDIT = 34,
};

#define BAUD 30

/* TODO: not all characters map to ascii */

#define XXX ((const char*)1)
#define Lcs ((const char*)2)
#define Ucs ((const char*)3)
/* 20-Jun-2026 wje: these used to be aliased to XXX, "shouldn't be sent".
 * They ARE sent now.
 * IOT_3.c (tyo) used to conflate case (Lcs/Ucs) and ribbon color (Blk/Red) by encoding case into a wire
 * bit and never forwarding 034/035 at all.
 * Real Flexowriter hardware had these as two independent shift mechanisms, so IOT_3.c now forwards
 * tb completely raw andthis table (which was already laid out with Blk/Red in the right slots) is what actually
 * implements ribbon-color tracking.
*/
#define Red ((const char*)4)
#define Blk ((const char*)5)

static const char *fio2uni[] = {
    " ", "1", "2", "3", "4", "5", "6", "7", "8", "9", XXX, XXX, XXX, XXX, XXX, XXX,
    "0", "/", "s", "t", "u", "v", "w", "x", "y", "z", XXX, ",", Blk, Red, "\t", XXX,
    "\xc2\xb7", "j", "k", "l", "m", "n", "o", "p", "q", "r", XXX, XXX, "-", ")", "\xe2\x80\xbe", "(",
    XXX, "a", "b", "c", "d", "e", "f", "g", "h", "i", Lcs, ".", Ucs, "\b", XXX, "\r\n",

    " ", "\"", "'", "~", "\xe2\x8a\x83", "\xe2\x88\xa8", "\xe2\x88\xa7", "<", ">", "\xe2\x86\x91", XXX, XXX, XXX, XXX, XXX, XXX,
    "\xe2\x86\x92", "?", "S", "T", "U", "V", "W", "X", "Y", "Z", XXX, "=", Blk, Red, "\t", XXX,
    "_", "J", "K", "L", "M", "N", "O", "P", "Q", "R", XXX, XXX, "+", "]", "|", "[",
    XXX, "A", "B", "C", "D", "E", "F", "G", "H", "I", Lcs, "\xc3\x97", Ucs, "\b", XXX, "\r\n",
};

/* 100 LC */
/* 200 UC */
static int ascii2fio[] = {
      -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    0075, 0036,   -1,   -1,   -1, 0077,   -1,   -1,
      -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
      -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,

    0000, 0205, 0201, 0204,   -1,   -1, 0206, 0202,
    0157, 0155, 0273, 0254, 0133, 0154, 0173, 0121,
    0120, 0101, 0102, 0103, 0104, 0105, 0106, 0107,
    0110, 0111,   -1,   -1, 0207, 0233, 0210, 0221,

    0140, 0261, 0262, 0263, 0264, 0265, 0266, 0267,
    0270, 0271, 0241, 0242, 0243, 0244, 0245, 0246,
    0247, 0250, 0251, 0222, 0223, 0224, 0225, 0226,
    0227, 0230, 0231, 0257, 0220, 0255, 0211, 0240,

    0156, 0161, 0162, 0163, 0164, 0165, 0166, 0167,
    0170, 0171, 0141, 0142, 0143, 0144, 0145, 0146,
    0147, 0150, 0151, 0122, 0123, 0124, 0125, 0126,
    0127, 0130, 0131,   -1, 0256,   -1, 0203,   -1,

/* missing  replacement
 * 204  superset-symbol  #
 * 205  or-symbol  !
 * 206  and-symbol  &
 * 220  right-arrow  backslash
 * 273  times-symbol  *
 * 140  middle-dot  @
 * 156  overline  backtick
 */
};

static int color;
static int ucase;
static FD *inputFdP;

// 20-Jun-2026 wje: dropped the `col` parameter and the wire-bit6-driven color switch.
// That encoded IOT_3.c's case state (tbb) into bit 6 of every byte and used it to drive the ANSI escape,
// conflating case and ribbon color which are independent on the real flexowriter.
// Color now changes only on a Blk/Red code from the table, exactly mirroring how
// ucase already only changes on a Lcs/Ucs code.
// The two state machines are now symmetric and independent, as they should be.
// With fd -1 nothing is written and only the case and ribbon state change, for output the relay
// discards: the Flexowriter's mechanism shifted whether or not anyone read the paper.
static void
putfio(int c, int fd)
{
    const char *s;
    ssize_t wr;

    c = ucase*0100 + (c&077);
    s = fio2uni[c];
    if(s == Lcs)
    {
        ucase = 0;
        return;
    }

    if(s == Ucs)
    {
        ucase = 1;
        return;
    }
    else if(s == Blk)
    {
        if( color && (fd >= 0) )
        {
            wr = write(fd, "\033[39;49m", 8); (void)wr;
        }
        color = 0;
        return;
    }
    else if(s == Red)
    {
        if( !color && (fd >= 0) )
        {
            wr = write(fd, "\033[31m", 5); (void)wr;
        }
        color = 1;
        return;
    }

    if( (s != XXX) && (fd >= 0) )
    {
        wr = write(fd, s, strlen(s)); (void)wr;
    }
}

static void
getfio(int c, int fd, int localfd)
{
    char s[2];
    int n;
    ssize_t wr;

    n = 0;
    if(c & 0300)
    {
        if(c & 0100 && ucase)
        {
            s[n++] = 072;
        }
        else if(c & 0200 && !ucase)
        {
            s[n++] = 074;
        }
    }

    s[n++] = c & 077;
    wr = write(fd, s, n);

    // Best-effort.
    // A dead peer is caught by the next read().
    // The tyi IOT reads only once ready is set and its read blocks,
    // so ready is set only after a write that landed.
    if( (wr > 0) && (inputFdP != nil) )
    {
        markFdReady(inputFdP);
    }

    // 20-Jun-2026 wje: was `putfio(color<<6 | s[i], localfd)`.
    // The color<<6 packing was a leftover of the old wire-bit6 color scheme.
    // Putfio() no longer looks at that bit at all, so it's just s[i] now.
    int i;
    for(i = 0; i < n; i++)
    {
        putfio(s[i], localfd);
    }
}


static void
getascii(int c, int fd, int localfd)
{
    // Simulate common combinations.
    // Didn't actually use to work so well, but maybe fixed now?
    if(c == ';')
    {
        getfio(0140, fd, localfd);
        getfio(033, fd, localfd);
    }
    else if(c == ':')
    {
        getfio(0140, fd, localfd);
        getfio(073, fd, localfd);
    }
    else
    {
        c = ascii2fio[c];
        if(c < 0)
        {
            return;
        }

        getfio(c, fd, localfd);
    }
}

// Reads one byte from the telnet client, waiting for it.
// Returns 1 with the byte in *cP, or 0 when the client has gone.
static int
readTelnetByte(int fd, unsigned char *cP)
{
ssize_t n;

    for(;;)
    {
        n = read(fd, cP, 1);
        if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }

        return( n == 1 );
    }
}

// Consumes the rest of a telnet command whose IAC has been read.
// Nothing is answered, the server's requests went out at connect
// and an option the client offers stays off unanswered.
static void
readiac(int fd)
{
unsigned char cc;

    if( !readTelnetByte(fd, &cc) )
    {
        return;
    }

    switch(cc)
    {
    case WILL:
    case WONT:
    case DO:
    case DONT:
        // The option code.
        readTelnetByte(fd, &cc);
        break;

    case SB:
        // A subnegotiation runs to IAC SE; inside it, IAC IAC is a data byte (RFC 855).
        while( readTelnetByte(fd, &cc) )
        {
            if( cc != IAC )
            {
                continue;
            }

            if( !readTelnetByte(fd, &cc) || (cc == SE) )
            {
                return;
            }
        }
        break;

    default:
        // IAC IAC, an escaped 0xFF data byte, has no Flexowriter code. NOP, DM, GA, BRK, IP, AO,
        // AYT, EC and EL mean nothing to the typewriter.
        break;
    }
}

// Relays between the telnet client on telfd and the emulator's socketpair end typfd until one
// of them ends.
// Returns 1 when the client has gone, 0 when typfd has (nothing more to serve).
static int
readwrite(int telfd, int typfd)
{
int n;
struct pollfd pfd[2];
char c;

    pfd[0].fd = typfd;
    pfd[0].events = POLLIN;
    pfd[1].fd = telfd;
    pfd[1].events = POLLIN;

    for(;;)
    {
        n = poll(pfd, 2, -1);
        if( n < 0 )
        {
            // A signal (SIGHUP reloads the configuration) is not the client leaving.
            if( errno == EINTR )
            {
                continue;
            }

            perror("error poll");
            return(1);
        }

        // A hangup or error with no data would wake every poll() from now on.
        if( (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) && !(pfd[0].revents & POLLIN) )
        {
            return(0);
        }

        if( (pfd[1].revents & (POLLHUP | POLLERR | POLLNVAL)) && !(pfd[1].revents & POLLIN) )
        {
            return(1);
        }

        /* take from pdp, send to telnet */
        if( pfd[0].revents & POLLIN )
        {
            if( read(typfd, &c, 1) <= 0 )
            {
                return(0);
            }

            putfio((c & 0177), telfd);
        }

        // receive over telnet, send to pdp
        if( pfd[1].revents & POLLIN )
        {
            if( read(telfd, &c, 1) <= 0 )
            {
                return(1);
            }

            // A byte with bit 7 set has no Flexowriter code and is dropped. Testing the bit, not
            // the sign, behaves the same whether char is signed (x86_64) or not (ARM).
            if( (c & 0377) == IAC )
            {
                readiac(telfd);
            }
            else if( !(c & 0200) )
            {
                getascii(c, typfd, telfd);
            }
        }
    }
}

static void
cmd(int fd, int a, int b)
{
    char ca = a;
    char cb = b;
    char iac = IAC;
    ssize_t wr;    // best-effort telnet negotiation write; dead peer caught downstream

    wr = write(fd, &iac, 1); (void)wr;
    wr = write(fd, &ca, 1); (void)wr;
    if(b >= 0)
    {
        wr = write(fd, &cb, 1); (void)wr;
    }
}

static int typport;
static int typfd;

// Discards what the program typed while no client was connected so a new client starts with
// current output, not a backlog that may be hours old.
// Each byte still passes through putfio()'s case and ribbon tracking.
// The relay is the only reader of fd, so this races nothing.
static void
drainTyped(int fd)
{
char buf[256];
ssize_t n;
int i;

    for(;;)
    {
        n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }

        // Would block (all read), or the emulator's end is gone, which readwrite() then finds.
        if( n <= 0 )
        {
            return;
        }

        for( i = 0; i < n; i++ )
        {
            putfio((buf[i] & 0177), -1);
        }
    }
}

// Serves the typewriter's telnet port, one client at a time, until the emulator's end of the
// socketpair is gone.
// A port that cannot listen is tried again every second.
// It reports once when it fails and once when a client is served again.
void*
telthread(void *arg)
{
ssize_t wr;
int telfd;
int reported;
(void)arg;

    reported = 0;

    for(;;)
    {
        telfd = serve1(typport);

        if( telfd < 0 )
        {
            if( !reported )
            {
                fprintf(stderr, "port %d: can't listen (%s), retrying\n", typport, strerror(errno));
                reported = 1;
            }

            sleep(1);
            continue;
        }

        if( reported )
        {
            fprintf(stderr, "port %d: serving again\n", typport);
            reported = 0;
        }

        // Before the negotiation, so a client that has received it gets nothing typed earlier.
        drainTyped(typfd);

        cmd(telfd, WILL, XMITBIN);
        cmd(telfd, DO, XMITBIN);
        cmd(telfd, WILL, ECHO_);
        cmd(telfd, DO, SUPRGA);
        cmd(telfd, WILL, SUPRGA);
        cmd(telfd, WONT, LINEEDIT);
        cmd(telfd, DONT, LINEEDIT);
        // Reset a fresh connection to default color if a previous connection left it red.
        // 20-Jun-2026 wje: was `putfio(0160, telfd)`, relying on putfio()'s old wire-bit6 color logic.
        // 0160's bit 6 is actually set, so that call never really reset anything even before this fix.
        // Now putfio() only changes color on an actual Blk/Red table entry.
        if(color)
        {
            color = 0;
            wr = write(telfd, "\033[39;49m", 8); (void)wr;
        }
        if( !readwrite(telfd, typfd) )
        {
            close(telfd);
            break;
        }

        close(telfd);
    }

    return(nil);
}

// Names the FD whose ready flag the relay sets after each write into the socketpair.
// Called before typtelnet().
void
typtelnetInput(FD *fdP)
{
    inputFdP = fdP;
}

void
typtelnet(int port, int fd)
{
    pthread_t th;
    typport = port;
    typfd = fd;
    pthread_create(&th, NULL, telthread, NULL);
}

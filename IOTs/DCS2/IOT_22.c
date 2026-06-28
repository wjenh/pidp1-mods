/*
 * This is a new implementation of the original Type 30 Data Control System using sockets instead of
 * serial connections. It keeps the general spirit of the original but modernized.
 * Some additional features have been added to deal with sockets.
 *
 * 14-Nov-2025 wje initial version, updates until 18-Jun-2026 were not recorded
 * 18-Jun-2026 wje rework client mode, now works correctly
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "iotHandler.h"
#include "flexlib.h"

#define NUM_CHANS   8       // the more chans, the higher the polling overhead
#define SERVER_BACKLOG  4   // number of incoming connect requests we queue

#define DOLOGGING
#include "iotLogger.h"

#define getFullAddress(pdp1P, addr) &(CORE(pdp1P)[(pdp1P->ema | (addr & 07777))%MAXMEM])
#define isTrue(f, t) (((f) & (t)) == (t))
#define hasChar(cP) (((cP->control_flags & CNTL_FLEX) && cP->flexo_rcv_pushback) || (cP->control_flags & CNTL_RREADY))
#define hasRcvPushback(cP) ((cP->control_flags & CNTL_FLEX) && cP->flexo_rcv_pushback)

#define CKS_CHAN_FLAG 02    // cks bit 16, channel locked

// Used in the flexo (actually concise) conversions, these are the flex lower/upper shift characters
#define CUNSHIFT   072
#define CSHIFT     074
#define FLEX_SPACE 000
// And the no-char-on-read marker
#define FLEX_NCHAR 013
#define ASCII_NCHAR 077
// We use 076 to indicate an error, it's not a Concise or Flex character
#define FLEX_ERR 076
// No character available on a read, or no flex->ascii translation on write, including shift codes
#define NONE -1

// No character available on a read, socket error
#define FAIL -2

// The original commands
#define RCH     000
#define RRC     001
#define RCR     010
#define RSC     011
#define TCB     040
#define SSB     041
#define TCC     050

// The extended commands
#define SCB     042
#define RLE     043
#define RPC     044
#define RCI     045
#define RIC     046
#define RCS     047
#define RWE     051
#define ROC     052
#define RES     053
#define RXL     054

#define SCBCLEAR    0
#define SCBOPEN     1
#define SCBREBIND   2
#define SCBRESET    4

// Control flags in the Channel structure.
// The 6 lowest also match the channel status bits.
#define CNTL_OPEN       0000001 // channel is open and in use
#define CNTL_SERVER     0000002 // this connection is a server, else a client
#define CNTL_CONNECTED  0000004 // a connection has been requested and established with the remote end
#define CNTL_TFULL      0000010 // the connection can't take more data
#define CNTL_RREADY     0000020 // there is input to be read
#define CNTL_CONNERR    0000040 // a socket error of some kind occurred
                     // 0000100    SBS
                     // 0000200    SBS
                     // 0000400    SBS
                     // 0001000    SBS
#define CNTL_LOST       0002000 // the remote end closed the connection

// These are common with bits in the control request word 0
#define CNTL_IE         0004000 // enable interrupts, SBS chan in REQ_SBS bits
#define CNTL_IOR        0010000 // interrupt on received characters ready or transmit buffer not full
#define CNTL_IOE        0020000 // interrupt on error including tbuf full
#define CNTL_IOC        0040000 // interrupt on connect, connection lost
#define CNTL_ECHO       0100000 // echo input to output
#define CNTL_FLEX       0200000 // Concise<->ascii translation is enabled
#define CNTL_CRLF       0400000 // convert a carraige return to carriage return and a line feed on output

// The reset mask is the bits we should clear when a channel is opened, in case there were leftovers
#define CNTL_RESET_MASK     (CNTL_LOST | CNTL_CONNERR | CNTL_TFULL | CNTL_RREADY)
#define CNTL_COMMON_MASK    0374000 // mask for the common bits with rcs

#define RQST_CRLF           0400000 // cr/lf bit in the scb control word
#define RQST_FLEX           0200000 // etc
#define RQST_ECHO           0100000
#define RQST_IOC            0040000
#define RQST_IOE            0020000
#define RQST_IOR            0010000
#define RQST_IE             0004000
#define RQST_SRV            0000100

// Request fields in word 0 of a channel control request not used above
#define REQ_CHAN_MSK    0000077     
#define REQ_SBS_MSK     0003600
#define REQ_SBS_SHIFT   7

// Statis bits we return in the IO register
#define STATUS_OPEN         0000001     // keep the first 6 aligned with the first 6 CNTL flags
#define STATUS_SERVER       0000002
#define STATUS_CONNECTED    0000004
#define STATUS_FULL         0000010     // transmit buffer full
#define STATUS_READ         0000020     // at least one character ready to read
#define STATUS_SOCKET       0000040     // a socket error of some kind happened

#define STATUS_LOST         0000100     // remote end closed the connection
#define STATUS_IE           0000200     // interrupts are enabled
#define STATUS_IOR          0000400     // interrupted because a character was received
#define STATUS_IOE          0001000     // interrupted because the transmit buffer is full or other error
#define STATUS_IOC          0002000     // interrupted because connection was opened or closed
#define STATUS_CHAN         0004000     // is current channel

// Error codes we return in the IO register
#define IO_ERR_FLAG         0400000 // bit 0, general error flag, no other bits set mean read returned 0 chars
#define IO_ERR_NOCHAR       0200000 // bit 1, if set a read returned no characters
#define IO_ERR_FULL         0100000 // bit 2, if set a send could not send all characters
#define IO_ERR_ERRNO        0040000 // bit 3, if set bits 6-13 contain a Linux errno

#define IO_ERR_NOTOPEN      0000001 // operation attempted on an unopened channel
#define IO_ERR_OPEN         0000002 // unallowed operation attempted on an open channel
#define IO_ERR_CHAN         0000003 // invalid channel number, exceeds NUM_CHANS
#define IO_ERR_NOTSERVER    0000004 // a server-specific command was issued on a non-server channel
#define IO_ERR_ILLEGAL      0000005 // illegal command, a request with invalid options was issued
#define IO_ERR_SOCKET       0000006 // server or client socket open failed or a socket operation failed
#define IO_ERR_BIND         0000007 // server bind to port failed
#define IO_ERR_NOCURRENT    0000010 // rch, rcr, tcb, or tcc done but no current channel and no wait
#define IO_ERR_LOST         0000011 // the remote end closed the connection
#define IO_ERR_EPOLL        0000012 // epoll() returned an error
#define IO_ERR_NOTCONNECTED 0000013 // no client connection

// This is used with epoll() to mark the type of poll entry
#define EP_SERVER       0100000

// These are flags in the IO register for RXL
#define RXL_FLEX            0400000
#define RXL_CHANGE          0001000
#define RXL_SHIFTED         0000400

typedef struct
{
int port;                   // the inet port assigned
int primary_fd;             // the server fd, from socket()
int count;                  // count of number of channels using this port
} PortMap, *PortMapP;

typedef struct
{
int chan_no;                // our channel number, for convenience
int chan_fd;                // the fd for the connection associated with this channel, -1 if not open
bool interrupt_issued;      // this chan requested an interrupt
int interrupts_in_process;  // the or of the CNTL_Ixx bit for interrupts in process
int interrupts_queued;      // pending interrupts, or of the CNTL_Ixx bits
int last_err;               // the last error value for this channel, 0 for none, reset by rce.
int control_flags;          // CNTL_x values, or'd
int sbs_chan;               // only if interrupts allowed for this channel
PortMapP primaryPortP;      // if a server channel, the port map entry associated with it
struct sockaddr_in address; // who we're talking to for a client channel
int flexo_snd_shift;        // we're doing flexo translation and a shift code is in effect on the sending side
int flexo_rcv_shift;        // we're doing flexo translation and a shift code is in effect on the receiving side
int flexo_rcv_pushback;     // we got a case change and returned a shift char, this is the pending real char
} Channel, *ChannelP;

static bool initialized;    // we have been started
static int epoll_fd = -1;   // used by epoll()
static int last_error;      // error from the last failed command regardless of channel

static int current_poll_interval = 20;  // default poll time, 100us
static int cur_chan = -1;       // which channel is currently selected, -1 for none
static bool cur_chan_locked;
static int send_chan = -1;      // if one was selected by ssb
static int last_intr_chan = -1; // last channel that interrupted
static int last_intr_reason;    // the CNTL_Ixx cause
static bool need_general_completion = false;    // need a completion pulse for a non-channel-specific operation

static Channel channels[NUM_CHANS];
static PortMap ports[NUM_CHANS];            // we will never have more ports than channels
struct epoll_event events[NUM_CHANS * 2];   // could be twice as many if all are unique server channels

Word manageChannelBlock(PDP1P , int);
void resetChannel(PDP1P , ChannelP);
PortMapP assignPort(PDP1P , int);
void releasePort(PortMapP);
void forceReleasePorts(void);
void closeRemoteSocket(ChannelP, int);
void releaseChannel(PDP1P );
bool canPost(PDP1P , ChannelP, int);
void postInterrupt(ChannelP, int);
int getChar(ChannelP);

extern int flexToAscii(char, int *);
extern char asciiToFlex(char, int *);

// Called when the emulator transitions to run state (START pressed or program loaded).
// Performs a full IOT state reset so that each new program run begins with a clean slate,
// regardless of how the previous run ended (normal HLT, console Stop, crash, etc.).
// Without this, stale state such as need_general_completion, open channel fds, or a
// pending SBS break from a previous run can corrupt the next program's execution.
void
iotStart()
{
int i;
ChannelP chanP;

    iotLog("DCS2 iotStart\n");

    if( initialized )
    {
        enablePolling(0);

        for( i = 0; i < NUM_CHANS; ++i )
        {
            chanP = &channels[i];
            if( (chanP->control_flags & CNTL_OPEN) && chanP->chan_fd != -1 )
            {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
                shutdown(chanP->chan_fd, SHUT_WR);
                close(chanP->chan_fd);
            }
            memset(chanP, 0, sizeof(Channel));
            chanP->chan_no = i;
            chanP->chan_fd = -1;
        }

        forceReleasePorts();

        if( epoll_fd != -1 )
        {
            close(epoll_fd);
            epoll_fd = -1;
        }

        last_error = 0;
        last_intr_chan = -1;
        last_intr_reason = 0;
        cur_chan = -1;
        cur_chan_locked = false;
        send_chan = -1;
        need_general_completion = false;
        initialized = false;

        iotCloseLog();
    }
}

void
iotStop()
{
    iotLog("DCS2 iotStop\n");
    iotCloseLog();
}

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
int i, j;
int cmd;
int ich;
bool clear_io;                          // bit 02000 was set, used by rcr and rch
ChannelP chanP;
struct epoll_event event;
char wbuf[8];

    if( pulse )
    {
        return(1);                  // only during TP7
    }

    if( !initialized )
    {
        iotLog("DCS2 initialized\n");

        if( (epoll_fd = epoll_create(1)) < 0 )
        {
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_EPOLL | ((errno & 0377) << 4);
            return(1);
        }

        enablePolling(20);              // every 20 cycles, 100us

        for( i = 0; i < NUM_CHANS; ++i )
        {
            chanP = &channels[i];
            chanP->chan_no = i;
            chanP->chan_fd = -1;
        }

        send_chan = -1;
        cur_chan = -1;
        need_general_completion = false;
        initialized = true;
    }

    cmd = (MB(pdp1P) >> 6) & 077;       // see what operation we do
    clear_io = cmd & 020;
    cmd &= ~020;
    chanP = 0;

    switch( cmd )                       // see what operation we do
    {
    case RCH:                           // single read
    case RCR:
        if( (cur_chan != -1) && cur_chan_locked )
        {
            chanP = &channels[cur_chan];

            // Clear the bits that will receive the character
            if( clear_io )
            {
                IO(pdp1P) = 0;
            }
            else
            {
                IO(pdp1P) &= (chanP->control_flags & CNTL_FLEX)?077:0xFF;
            }

            if( hasRcvPushback(chanP) )
            {
                ich = getChar(chanP);
                chanP->control_flags |= CNTL_RREADY;    // just to be sure
            }
            else if( !hasChar(chanP)  || ((ich = getChar(chanP)) < 0) )
            {
                // no char or error, set flag bits for binary, FLEX_NCHAR or FLEX_ERR if Flex mode
                if( chanP->control_flags & CNTL_FLEX )
                {
                    ich = (ich == FAIL)?FLEX_ERR:FLEX_NCHAR;
                }
                else
                {
                    ich = IO_ERR_FLAG | ((ich == FAIL)?IO_ERR_LOST:IO_ERR_NOCHAR);
                }
            }

            // getChar() might have added pushback
            if( !hasRcvPushback(chanP) )
            {
                chanP->control_flags &= ~CNTL_RREADY;   // will be reset in poll loop
            }

            IO(pdp1P) |= ich;
        }
        else
        {
            // No active channel, return an error.
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_NOCURRENT;
        }

        if( cmd == RCR ) 
        {
            releaseChannel(pdp1P);
            CKS(pdp1P) &= ~CKS_CHAN_FLAG;
        }
        break;

    case RRC:                                   // get current channel number, if any
        last_error = IO(pdp1P) = (cur_chan == -1)?IO_ERR_FLAG | IO_ERR_NOCURRENT:cur_chan;
        break;

    case RSC:                                   // release current channel, if any
        IO(pdp1P) = 0;
        releaseChannel(pdp1P);
        break;

    case TCB:                                   // single write to send chan, if none, use current chan
        if( send_chan >= 0 )
        {
            chanP = &channels[send_chan];
        }
        else
        {
            iotLog("TCB no send_chan, using cur_chan\n");
            chanP = 0;
        }
        // fall into TCC
    case TCC:                                   // single write
        if( !chanP )
        {
            if( cur_chan < 0 )
            {
                iotLog("TCC/TCB has no channel assigned\n");
                last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_NOCURRENT;
                break;
            }
            else
            {
                chanP = &channels[cur_chan];
            }
        }

        if( !(chanP->control_flags & CNTL_CONNECTED) )
        {
            iotLog("TCC/TCB channel %d is not connected\n", cur_chan);
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_NOTCONNECTED;
            break;
        }

        if( chanP->chan_fd < 0 )
        {
            iotLog("TCC/TCB channel %d has no fd\n", cur_chan);
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_NOCURRENT;
            break;
        }

        if( chanP->control_flags & CNTL_TFULL )
        {
            iotLog("TCC/TCB has FULL on %d\n", cur_chan);
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_FULL;
        }
        else
        {
            if( chanP->control_flags & CNTL_FLEX )
            {
                ich = flexToAscii(IO(pdp1P) & 077, &(chanP->flexo_snd_shift));

                if( ich == NONE )
                {
                    break;                      // pretend we wrote something
                }
            }
            else
            {
                ich = IO(pdp1P) & 0xFF;
            }

            if( (ich == '\n') && (chanP->control_flags & CNTL_CRLF) )
            {
                wbuf[0] = '\r';
                wbuf[1] = ich;
                i = 2;
            }
            else
            {
                wbuf[0] = ich;
                i = 1;
            }

            j = send(chanP->chan_fd, wbuf, i, MSG_NOSIGNAL);

            if( j < 0 )
            {
                if( errno == EAGAIN )
                {
                    iotLog("TCC/TCB got EAGAIN on %d\n", cur_chan);
                    chanP->control_flags |= CNTL_TFULL;
                    last_error = IO(pdp1P) |= IO_ERR_FLAG | IO_ERR_FULL;

                    if( canPost(pdp1P, chanP, CNTL_IOE) )
                    {
                        postInterrupt(chanP, CNTL_IOE);
                    }

                    // we now want notification when we can write again.
                    event.events = EPOLLIN | EPOLLOUT;
                    event.data.u32 = chanP->chan_no;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, chanP->chan_fd, &event);
                }
                else                    // an error, probably no remote anymore
                {
                    iotLog("TCC/TCB errno %d on %d\n", errno, cur_chan);
                    last_error = chanP->last_err = IO_ERR_FLAG | IO_ERR_ERRNO | errno;
                    break;
                }
            }
            else
            {
                IO(pdp1P) = 0 ;           // succeeded
            }
        }
        break;

    case SSB:                           // select a send channel
        i = IO(pdp1P) & 077;
        if( i >= NUM_CHANS )
        {
            last_error = IO(pdp1P) |= IO_ERR_FLAG | IO_ERR_CHAN;
        }
        else
        {
            send_chan = i;
        }
        break;

    case SCB:                           // extended command, configure channel
        IO(pdp1P) = manageChannelBlock(pdp1P, IO(pdp1P));
        if( IO(pdp1P) & IO_ERR_FLAG )   // propagate error to last_error for RLE
            last_error = IO(pdp1P);
        break;

    case RLE:                           // extended command, get last error
        IO(pdp1P) = last_error;
        last_error = 0;
        break;

    case RPC:                           // extended command, get chars ready to read
        i = IO(pdp1P) & 077;            // the channel number

        if( i >= NUM_CHANS )
        {
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_CHAN;
        }
        else
        {
            chanP = &channels[i];
            ioctl(chanP->chan_fd, FIONREAD, &(IO(pdp1P)));
        }
        break;

    case RCI:                           // extended command, clear interrupt
        i = IO(pdp1P) & 077;
        if( i >= NUM_CHANS )
        {
            last_error = IO(pdp1P) |= IO_ERR_FLAG | IO_ERR_CHAN;
        }
        else
        {
            chanP = &channels[i];
            if( chanP->control_flags & CNTL_IE )
            {
                iotLog("RCI resetting interrupts for channel %d\n", i);

                chanP->interrupt_issued = false;
                chanP->interrupts_in_process = 0;
                chanP->interrupts_queued = 0;
            }

            if( last_intr_chan == i)
            {
                last_intr_chan = -1;
            }
        }
        break;

    case RIC:                           // extended command, get last chan that interrupted
        if( last_intr_chan == -1 )
        {
            IO(pdp1P) = 0100;
        }
        else
        {
            IO(pdp1P) = last_intr_chan;
        }
        break;

    case RCS:                           // get channel status
        i = IO(pdp1P) & 077;            // the channel number

        if( i >= NUM_CHANS )
        {
            IO(pdp1P) = IO_ERR_FLAG | IO_ERR_CHAN;    // bad channel
        }
        else
        {
            chanP = &channels[i];
            IO(pdp1P) = chanP->control_flags & 077;     // be sure status and control flags say aligned!

            if( chanP->control_flags & CNTL_IE )
            {
                IO(pdp1P) |= STATUS_IE;
            }

            if( chanP->control_flags & CNTL_LOST )
            {
                IO(pdp1P) |= STATUS_LOST;
            }

            if( i == cur_chan )
            {
                IO(pdp1P) |= STATUS_CHAN;
            }

            if( i == last_intr_chan )
            {
                if( last_intr_reason & CNTL_IOR )
                {
                    IO(pdp1P) |= STATUS_IOR;
                }

                if( last_intr_reason & CNTL_IOE )
                {
                    IO(pdp1P) |= STATUS_IOE;
                }

                if( last_intr_reason & CNTL_IOC )
                {
                    IO(pdp1P) |= STATUS_IOC;
                }
            }
        }
        break;

    case RWE:                           // wait for any event
        iotLog("RWE called, completion %d, io %d\n", completion, IO(pdp1P));
        if( completion )
        {
            need_general_completion = true;
            iotLog("RWE set wait\n");
        }
        break;

    case ROC:                           // override current channel
        i = IO(pdp1P) & 077;            // the channel number

        if( i >= NUM_CHANS )
        {
            IO(pdp1P) = IO_ERR_FLAG | IO_ERR_CHAN;    // bad channel
        }
        else
        {
            chanP = &channels[i];
            if( !(chanP->control_flags & CNTL_OPEN) )
            {
                IO(pdp1P) = IO_ERR_FLAG | IO_ERR_NOTOPEN;
            }
            else
            {
                cur_chan = i;
                cur_chan_locked = true;
                CKS(pdp1P) |= CKS_CHAN_FLAG;
                IO(pdp1P) = 0;
            }
        }
        break;

    case RES:                   // enable/disable sbs16
        i = !!pdp1P->sbs16;     // in case it wasn't just 1
        pdp1P->sbs16 = IO(pdp1P) & 1;
        IO(pdp1P) = i;
        break;

    case RXL:                   // translate to/from flexo and ascii
        i = IO(pdp1P) & RXL_SHIFTED;    // shift flag

        if( IO(pdp1P) & RXL_FLEX )      // flex->ascii
        {
            ich = flexToAscii(IO(pdp1P) & 077, &i);
            if( ich == NONE )
            {
                ich = ASCII_NCHAR;
            }
        }
        else
        {
            ich = asciiToFlex(IO(pdp1P) & 0377, &i);
            if( ich == NONE )
            {
                ich = FLEX_NCHAR;
            }
        }

        if( i )
        {
            i = RXL_SHIFTED;
        }

        if( (IO(pdp1P) & RXL_SHIFTED) != i )
        {
            IO(pdp1P) |= RXL_CHANGE;
        }
        else
        {
            IO(pdp1P) &= ~RXL_CHANGE;
        }

        IO(pdp1P) &= ~(RXL_SHIFTED | 0377);    // clear full 8-bit ASCII field
        IO(pdp1P) |= (i | ich);
        break;

    default:
        return(0);              // unknown
    }

    if( completion && !need_general_completion )
    {
        IOCOMPLETE(pdp1P); // we are finished already, we don't block at all
    }

    return(1);
}

// Used to update channels, etc.
void
iotPoll(PDP1P pdp1P)
{
int i, j;
int data;
bool did_our_event;
ChannelP chanP;
PortMapP mapP;
struct epoll_event *eventP;
struct epoll_event event;

    if( (i = epoll_wait(epoll_fd, events, NUM_CHANS * 2, 0)) )
    {
        // We can have a connection request on server chans or data ready on client chans
        eventP = events;
        did_our_event = false;

        while( i-- )
        {
            data = eventP->data.u32;

            if( data & EP_SERVER )                  // connection request
            {
                data &= ~EP_SERVER;

                if( eventP->events & EPOLLIN )
                {
                    did_our_event = true;
                    mapP = &ports[data];
                    // Find the first available channel that is associated with this map entry
                    chanP = &channels[0];
                    for( j = 0; j++ < NUM_CHANS; ++chanP )
                    {
                        if( ((chanP->control_flags & (CNTL_SERVER | CNTL_CONNECTED)) == CNTL_SERVER) &&
                            (chanP->primaryPortP == mapP) )
                        {
                            // Open but not connected, means waiting for a connect
                            if( (chanP->chan_fd = accept(mapP->primary_fd, NULL, NULL)) < 0 )
                            {
                                // Hmm, not good.
                                chanP->control_flags |= CNTL_CONNERR;
                                last_error = chanP->last_err =
                                    IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((errno & 0377) << 4);
                                if( canPost(pdp1P, chanP, CNTL_IOE) )
                                {
                                    iotLog("Posting IOE %o on chan %d\n", last_error, chanP->chan_no);
                                    postInterrupt(chanP, CNTL_IOE);
                                }
                            }
                            else
                            {
                                chanP->control_flags |= CNTL_CONNECTED;
                                chanP->control_flags &= ~CNTL_RESET_MASK;

                                j = fcntl(chanP->chan_fd, F_GETFL, 0);
                                j |= O_NONBLOCK | O_RDWR;
                                fcntl(chanP->chan_fd, F_SETFL, j);

                                event.events = EPOLLIN | EPOLLRDHUP;    // EPOLLRDHUP detects orderly close
                                event.data.u32 = chanP->chan_no;
                                j = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, chanP->chan_fd, &event);

                                iotLog("Client connected to channel %d\n", chanP->chan_no);
                                if( canPost(pdp1P, chanP, CNTL_IOC) )
                                {
                                    iotLog("Posting IOC connected on chan %d\n",chanP->chan_no);
                                    postInterrupt(chanP, CNTL_IOC);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            else
            {
                // data will be our channel number
                chanP = &channels[data];

                if( eventP->events & EPOLLIN )             // data ready on chan or a connect is pending
                {
                    did_our_event = true;
                    if( chanP->control_flags & CNTL_CONNECTED )
                    {
                        if( eventP->events & EPOLLRDHUP )
                        {
                            // Orderly remote shutdown (FIN received): treat as connection close.
                            // On Linux, EPOLLIN | EPOLLRDHUP fires together when the peer calls close().
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
                            close(chanP->chan_fd);
                            chanP->chan_fd = -1;
                            chanP->control_flags &= ~(CNTL_CONNECTED | CNTL_TFULL);
                            chanP->control_flags |= CNTL_LOST;
                            last_error = chanP->last_err = IO_ERR_FLAG | IO_ERR_LOST;

                            if( canPost(pdp1P, chanP, CNTL_IOC) )
                            {
                                iotLog("Posting IOC orderly close on chan %d\n", chanP->chan_no);
                                postInterrupt(chanP, CNTL_IOC);
                            }
                        }
                        else
                        {
                            chanP->control_flags |= CNTL_RREADY;
                            if( (cur_chan < 0) || !cur_chan_locked )
                            {
                                cur_chan = data;
                                cur_chan_locked = true;
                                CKS(pdp1P) |= CKS_CHAN_FLAG;
                            }

                            if( canPost(pdp1P, chanP, CNTL_IOR) )
                            {
                                iotLog("Posting IOR on channel %d\n", data);
                                postInterrupt(chanP, CNTL_IOR);
                            }
                        }
                    }
                    else
                    {
                        chanP->control_flags |= CNTL_CONNECTED;
                        chanP->control_flags &= ~CNTL_RESET_MASK;
                        if( canPost(pdp1P, chanP, CNTL_IOC) )
                        {
                            iotLog("Posting IOC connected on chan %d\n",chanP->chan_no);
                            postInterrupt(chanP, CNTL_IOC);
                        }
                    }
                }
                else if( eventP->events & EPOLLOUT )    // transmit buffer not full, or client connect() completed
                {
                struct epoll_event newEvent;
                int soErr;
                socklen_t soErrLen;

                    did_our_event = true;

                    if( !(chanP->control_flags & CNTL_CONNECTED) &&
                        !(chanP->control_flags & CNTL_SERVER) )
                    {
                        // Non-blocking connect() completion for a client channel.
                        // EPOLLOUT fires when the connection is established or fails.
                        // Use getsockopt(SO_ERROR) to determine which.
                        soErr = 0;
                        soErrLen = sizeof(soErr);
                        getsockopt(chanP->chan_fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen);

                        if( soErr == 0 )
                        {
                            // Connection established successfully.
                            chanP->control_flags |= CNTL_CONNECTED;
                            chanP->control_flags &= ~CNTL_RESET_MASK;

                            // Switch to EPOLLIN | EPOLLRDHUP; EPOLLOUT re-enabled on buffer full.
                            newEvent.events = EPOLLIN | EPOLLRDHUP;
                            newEvent.data.u32 = chanP->chan_no;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, chanP->chan_fd, &newEvent);

                            iotLog("Client channel %d connected\n", chanP->chan_no);

                            if( canPost(pdp1P, chanP, CNTL_IOC) )
                            {
                                iotLog("Posting IOC connected on chan %d\n", chanP->chan_no);
                                postInterrupt(chanP, CNTL_IOC);
                            }
                        }
                        else
                        {
                            // Connection failed.
                            chanP->control_flags |= CNTL_CONNERR;
                            last_error = chanP->last_err =
                                IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((soErr & 0377) << 4);
                            iotLog("Client channel %d connect failed, errno %d\n",
                                chanP->chan_no, soErr);

                            if( canPost(pdp1P, chanP, CNTL_IOE) )
                            {
                                iotLog("Posting IOE connect failed on chan %d\n", chanP->chan_no);
                                postInterrupt(chanP, CNTL_IOE);
                            }
                        }
                    }
                    else
                    {
                        // Connected channel: transmit buffer is no longer full.
                        chanP->control_flags &= ~CNTL_TFULL;

                        // Turn off EPOLLOUT until the buffer fills again.
                        newEvent.events = EPOLLIN | EPOLLRDHUP;
                        newEvent.data.u32 = chanP->chan_no;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, chanP->chan_fd, &newEvent);

                        if( canPost(pdp1P, chanP, CNTL_IOR) )
                        {
                            iotLog("Posting IOR buffer available on chan %d\n", chanP->chan_no);
                            postInterrupt(chanP, CNTL_IOR);
                        }
                    }
                }
                else if( eventP->events & EPOLLHUP )    // connection closed by remote end
                {
                    did_our_event = true;

                    // Remove from epoll and close the fd immediately.  Without this,
                    // epoll_wait() would return EPOLLHUP on every call, creating a
                    // polling storm while the fd remains registered.
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
                    close(chanP->chan_fd);
                    chanP->chan_fd = -1;
                    chanP->control_flags &= ~(CNTL_CONNECTED | CNTL_TFULL);
                    chanP->control_flags |= CNTL_LOST;
                    last_error = chanP->last_err = IO_ERR_FLAG | IO_ERR_LOST;

                    if( canPost(pdp1P, chanP, CNTL_IOC) )
                    {
                        iotLog("Posting IOC connection lost on chan %d\n",chanP->chan_no);
                        postInterrupt(chanP, CNTL_IOC);
                    }
                }
                else if( eventP->events & EPOLLERR )    // some error on the connection
                {
                    did_our_event = true;

                    // Same cleanup as EPOLLHUP to prevent a polling storm.
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
                    close(chanP->chan_fd);
                    chanP->chan_fd = -1;
                    chanP->control_flags &= ~(CNTL_CONNECTED | CNTL_TFULL);
                    chanP->control_flags |= CNTL_CONNERR;
                    last_error = chanP->last_err = IO_ERR_FLAG | IO_ERR_SOCKET;

                    if( canPost(pdp1P, chanP, CNTL_IOE) )
                    {
                        iotLog("Posting IOE socket error on chan %d\n",chanP->chan_no);
                        postInterrupt(chanP, CNTL_IOE);
                    }
                }

                if( (chanP->control_flags & CNTL_TFULL) && (chanP->chan_fd >= 0) )
                {
                struct epoll_event tfEvent;

                    // Re-register EPOLLOUT so we learn when the buffer drains.
                    tfEvent.events = EPOLLIN | EPOLLOUT;
                    tfEvent.data.u32 = chanP->chan_no;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, chanP->chan_fd, &tfEvent);
                }
            }
        }

        if( need_general_completion && did_our_event )   // rwe is waiting
        {
            iotLog("Posting completion for rwe\n");
            IOCOMPLETE(pdp1P);
            need_general_completion = false;
        }
    }
}

Word
manageChannelBlock(PDP1P pdp1P, int io)
{
int i;
int cmd;
int chan_no;
int port;
Word *rqstP;
Word word;
ChannelP chanP;
struct epoll_event event;

    cmd = (io >> 12) & 07;

    if( cmd != SCBRESET )
    {
        if( cmd == SCBREBIND )
        {
            chan_no = io & 07777;
        }
        else
        {
            rqstP = getFullAddress(pdp1P, io & 07777);
            word = (int)*rqstP++;
            chan_no = word & REQ_CHAN_MSK;
        }

        if( chan_no >= NUM_CHANS )
        {
            iotLog("manageChannels got bad channel %d\n", chan_no);
            return( IO_ERR_FLAG | IO_ERR_CHAN );                  // nope
        }

        chanP = &channels[chan_no];
    }

    switch( cmd )
    {
    case SCBCLEAR:
        iotLog("Channel close on %d\n", chan_no);
        resetChannel(pdp1P, chanP);
        break;

    case SCBOPEN:
        if( chanP->control_flags & CNTL_OPEN )
        {
            iotLog("set channel, already open\n");
            return( IO_ERR_FLAG | IO_ERR_OPEN );              // already open
        }

        iotLog("Channel open on %d\n", chan_no);

        chanP->control_flags = (word & RQST_SRV)?CNTL_SERVER:0;

        chanP->control_flags |= (word & RQST_IE)?CNTL_IE:0;
        chanP->control_flags |= (word & RQST_IOR)?CNTL_IOR:0;
        chanP->control_flags |= (word & RQST_IOE)?CNTL_IOE:0;
        chanP->control_flags |= (word & RQST_IOC)?CNTL_IOC:0;
        chanP->control_flags |= (word & RQST_ECHO)?CNTL_ECHO:0;
        chanP->control_flags |= (word & RQST_FLEX)?CNTL_FLEX:0;
        chanP->control_flags |= (word & RQST_CRLF)?CNTL_CRLF:0;

        port = *rqstP++;

        if( word & RQST_SRV )
        {
            chanP->address.sin_family = AF_INET;
            chanP->address.sin_addr.s_addr = INADDR_ANY;
            chanP->address.sin_port = htons(port);

            if( !(chanP->primaryPortP = assignPort(pdp1P, port)) )
            {
                iotLog("set channel, assignPort failed, errno %d\n", errno);
                last_error = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((errno & 0377) << 4);
                chanP->last_err = last_error;
                return( last_error );
            }
            
            chanP->control_flags |= CNTL_OPEN;      // poll will establish the connection
            iotLog("channel %d open in server mode, waiting for a connection\n", chan_no);
        }
        else
        {
            i = *rqstP++;               // word 2, netaddr high bits
            i = (i << 16) | *rqstP;     // word 3, netaddr low bits

            if ((chanP->chan_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
            {
                last_error = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((errno & 0377) << 4);
                chanP->last_err = last_error;
                return( last_error );
            }

            chanP->address.sin_family = AF_INET;
            chanP->address.sin_addr.s_addr = htonl(i);
            chanP->address.sin_port = htons(port);
            chanP->control_flags |= CNTL_OPEN;

            // Register with epoll before calling connect() so that the connection-
            // completion EPOLLOUT event is not missed between the two calls.
            event.events = EPOLLIN | EPOLLOUT;
            event.data.u32 = chan_no;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, chanP->chan_fd, &event);   // op and fd were swapped

            // Initiate the non-blocking connection.  EINPROGRESS is expected and
            // normal; the actual result is signalled via EPOLLOUT in iotPoll().
            if( connect(chanP->chan_fd,
                        (struct sockaddr *)&chanP->address,
                        sizeof(chanP->address)) < 0 )
            {
                if( errno != EINPROGRESS )
                {
                    // Immediate failure — clean up and report.
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
                    close(chanP->chan_fd);
                    chanP->chan_fd = -1;
                    chanP->control_flags &= ~CNTL_OPEN;
                    last_error = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((errno & 0377) << 4);
                    chanP->last_err = last_error;
                    return( last_error );
                }
                // EINPROGRESS: connection is underway; iotPoll() will handle completion.
                iotLog("channel %d connect() in progress\n", chan_no);
            }
            else
            {
                // Immediate connect() success (e.g., loopback).
                chanP->control_flags |= CNTL_CONNECTED;
                chanP->control_flags &= ~CNTL_RESET_MASK;
                iotLog("channel %d connected immediately\n", chan_no);
            }
        }

        if( word & RQST_IE )
        {
            chanP->sbs_chan = (word & REQ_SBS_MSK)  >> REQ_SBS_SHIFT;
        }
        break;

    case SCBREBIND:
        iotLog("Channel rebind on %d\n", chan_no);
        if( !isTrue(chanP->control_flags,(CNTL_OPEN|CNTL_SERVER)) )
        {
            iotLog("Channel rebind on %d, not open and not server\n", chan_no);
            return( IO_ERR_FLAG | IO_ERR_NOTSERVER );              // not open or not server
        }

        if( chanP->chan_fd != -1 )
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
            shutdown(chanP->chan_fd, SHUT_WR);
            close( chanP->chan_fd );
            chanP->chan_fd = -1;
        }

        // now available, poll will take it from here
        chanP->control_flags  &= ~(CNTL_CONNECTED|CNTL_RREADY|CNTL_TFULL|CNTL_CONNERR);
        break;

    case SCBRESET:
        enablePolling(0);
        iotLog("DCS full reset\n");

        for( i = 0; i < NUM_CHANS; ++i )
        {
            resetChannel(pdp1P, &channels[i]);
        }

        forceReleasePorts();

        if( epoll_fd != -1 )
        {
            close( epoll_fd );
            epoll_fd = -1;
        }

        iotCloseLog();              // just to keep the log file updated
        initialized = false;
        break;

    default:
        return( IO_ERR_FLAG | IO_ERR_ILLEGAL ); 
    }

    return(0);
}

// Completely reset a channel.
// This will also close its fd, if any and if a server chan, remove it from the port map.
// If this is the last channel that is assigned to a particular port, the port's primary
// file descriptor is also closed and no more connections will be accepted until a new
// channel setup is done to accept the port.
void
resetChannel(PDP1P pdp1P, ChannelP chanP)
{
int i;

    iotLog("Resetting channel %d\n", chanP->chan_no);

    if( !(chanP->control_flags & CNTL_OPEN) )
    {
        iotLog("Reset channel but not open\n");
        return;                      // ignore
    }

    if( chanP->chan_fd != -1 )
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
        shutdown(chanP->chan_fd, SHUT_WR);
        close( chanP->chan_fd );
    }

    if( chanP->control_flags & CNTL_SERVER )
    {
        releasePort(chanP->primaryPortP);
    }

    i = chanP->chan_no;
    memset(chanP, 0, sizeof(Channel));
    chanP->chan_no = i;                 // we keep these settings
    chanP->chan_fd = -1;                // and initialize this

    if( cur_chan == i )
    {
        cur_chan = -1;
        cur_chan_locked = 0;
        CKS(pdp1P) &= ~CKS_CHAN_FLAG;
    }

    if( send_chan == i )
    {
        send_chan = -1;
    }

    if( last_intr_chan == i )
    {
        last_intr_chan = -1;
    }
}

// See if interrupts are enabled, none in progress, and this CNTL_Ixx type is allowed.
// If allowed but one is already in process, just set the queued flag.
bool
canPost(PDP1P pdp1P, ChannelP chanP, int kind)
{
    if( pdp1P->sbm && isTrue(chanP->control_flags, (CNTL_IE | kind)) )
    {
        if( chanP->interrupt_issued )   // chan is enabled but in an interrupt now, postponed?
        {
            if( !(chanP->interrupts_in_process & kind) && !(chanP->interrupts_queued & kind) )
            {
                // We haven't seen this interrupt, queue it
                iotLog("canPost queueing interrupt %o for chan %d\n", kind, chanP->chan_no);
                chanP->interrupts_queued |= kind;
            }
        }
        else
        {
            iotLog("canPost says yes for channel %d\n", chanP->chan_no);
            return( true );
        }
    }

    return( false );
}

// Post an interrupt request if needed.
// The channel status should already be updated.
// Returns true if an interrrupt was posted, else false
void
postInterrupt(ChannelP chanP, int kind)
{
    if( (chanP->control_flags & CNTL_IE) && !chanP->interrupt_issued )
    {
        iotLog("postInterrupt interrupt %o for chan %d\n", kind, chanP->chan_no);
        initiateBreak(chanP->sbs_chan);
        chanP->interrupt_issued = true;
        chanP->interrupts_in_process |= kind;
        last_intr_reason = chanP->interrupts_in_process;
        last_intr_chan = chanP->chan_no;
    }
}

// Fetch the next char from the channel's fd.
// If echo needed, echo it.
// If in Flexo, translate it.
// If a shift is needed, buffer the real character and return the shift code.
// If there is no character to read, return NONE, -1, or FAIL, -2.
int
getChar(ChannelP chanP)
{
int i;
char ch, ch2;

    if( hasRcvPushback(chanP) )
    {
        ch = chanP->flexo_rcv_pushback;
        chanP->flexo_rcv_pushback = 0;
    }
    else
    {
        if( !(chanP->control_flags & CNTL_CONNECTED) )
        {
            return( FAIL );
        }

        i = recv(chanP->chan_fd, &ch, sizeof(ch), 0);

        if( i == 0 )
        {
            // recv() returning 0 means the peer performed an orderly shutdown (TCP FIN).
            // This is not "no data" -- it is a connection close.  Returning NONE would
            // leave CNTL_RREADY set, causing epoll to keep firing with nothing to read.
            iotLog("getChar() recv() returned 0, remote closed connection\n");
            closeRemoteSocket(chanP, 0);
            return( FAIL );
        }
        else if( i < 0 )
        {
            if( errno == EAGAIN )
            {
                iotLog("getChar() recv() has no data\n");
                return(NONE);
            }
            else
            {
                iotLog("getChar() recv() returned %d\n", errno);
                closeRemoteSocket(chanP, errno);
                return(FAIL);
            }
        }

        if( chanP->control_flags & CNTL_ECHO )
        {
            send(chanP->chan_fd, &ch, sizeof(ch), MSG_NOSIGNAL);
        }
    }

    if( chanP->control_flags & CNTL_FLEX )
    {
        if( ch == '\n' )        // drop linefeed, doesn't exist in Concise
        {
            ch2 = FLEX_NCHAR;
        }
        else
        {
            // If we have a char and it isn't a newline, convert it
            ch2 = asciiToFlex(ch, &(chanP->flexo_rcv_shift));

            if( ch2 == NONE )
            {
                ch2 = FLEX_NCHAR;       // concise mapping
            }
            else if( (ch2 == CSHIFT) || (ch2 == CUNSHIFT) )
            {
                // return the shift char, save the char for next time
                chanP->flexo_rcv_pushback = ch;
            }
        }
    }
    else
    {
        ch2 = ch;
    }

    return( ch2 );
}

void
releaseChannel(PDP1P pdp1P)
{
int i;
int old_chan;
ChannelP chanP;

    // Save the channel being released so the scan can start from the next one.
    // cur_chan must be cleared BEFORE the scan so that if no ready channel is
    // found, it remains -1.  Using cur_chan after clearing it caused the loop
    // termination test (i != cur_chan, i.e. i != -1) to be always true.
    old_chan = cur_chan;
    cur_chan = -1;
    cur_chan_locked = false;
    CKS(pdp1P) &= ~CKS_CHAN_FLAG;

    if( old_chan < 0 )
    {
        return;                 // nothing was current, nothing to scan from
    }

    // Scan the channel list starting after the just-released channel.
    // If a channel that is open and has data ready is found, make it current.
    // If none is found the loop exits cleanly and poll will select the next one.
    for( i = (old_chan + 1) % NUM_CHANS; i != old_chan; i = (i + 1) % NUM_CHANS )
    {
        chanP = &channels[i];
        if( (chanP->control_flags & (CNTL_OPEN|CNTL_RREADY)) == (CNTL_OPEN|CNTL_RREADY) )
        {
            cur_chan = i;
            cur_chan_locked = true;
            CKS(pdp1P) |= CKS_CHAN_FLAG;
            break;
        }
    }
}

// Given a port, see if it already has a map entry.
// If not, assign one and establish the prirmary fd (from socket()).
// Increment the map entry's port-used count.
// On error, return 0, else the assigned PortMapP.
// Both the channel and IO register will already be set with the error.
PortMapP
assignPort(PDP1P pdp1P, int port)
{
int i;
int empty;
int portFd;
PortMapP mapP;
struct sockaddr_in address;
struct epoll_event event;

    // Find an avalable channel for the port
    for( empty = -1, i = 0; i < NUM_CHANS; ++i )
    {
        mapP = &ports[i];
        if( mapP->port == port )
        {
            empty = -1;         // in case we saw an empty slot, we're not using it, we're using the assigned one
            break;
        }
        else if( !mapP->port && (empty < 0) )
        {
            empty = i;          // we will use this if we have to open one
        }
    }

    if( empty >= 0 )                 // first use, allocate the primary fd
    {
        mapP = &ports[empty];
        mapP->port = port;

        if ((mapP->primary_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
        {
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_SOCKET | ((errno & 0377) << 4);
            return( 0 );
        }

        i = 1;
        setsockopt(mapP->primary_fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i) );

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if( bind(mapP->primary_fd, (struct sockaddr*)&address, sizeof(address)) < 0 )
        {
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_BIND | ((errno & 0377) << 4);
            return( 0 );
        }

        if( listen(mapP->primary_fd, SERVER_BACKLOG) < 0 )
        {
            last_error = IO(pdp1P) = IO_ERR_FLAG | IO_ERR_ERRNO | IO_ERR_BIND | ((errno & 0377) << 4);
            return( 0 );
        }

        event.events = EPOLLIN;
        event.data.u32 = EP_SERVER | empty;     // primary server fd, keep the map slot number
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, mapP->primary_fd, &event);
    }

    mapP->count++;
    return( mapP );
}

// Given a port's map entry, release it if it is no longer in use.
// The associated count is decremented and if it becomes 0, the primary_fd is closed
// and the entry made empty.
void
releasePort(PortMapP mapP)
{
    if( --(mapP->count) <= 0 )
    {
        close( mapP->primary_fd );
        mapP->port = 0;
        mapP->count = 0;
        mapP->primary_fd = -1;
    }
}

// For server ports only.
// Close every open map entry regardless of use count.
void
forceReleasePorts()
{
int i;
PortMapP mapP;

    for( i = 0; i < NUM_CHANS; ++i )
    {
        mapP = &ports[i];
        if( mapP->port )
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, mapP->primary_fd, 0);
            close( mapP->primary_fd );

            mapP->port = 0;
            mapP->count = 0;
            mapP->primary_fd = -1;
        }
    }
}

// Remote side failed on recv(), close things up.
// The channel becomes available for a new connection (server) or stays closed (client).
// Note: releasePort() is NOT called here.  For server channels the listen socket must
// remain active so that a new client can connect; the port map entry is only torn down
// by resetChannel().  For client channels, primaryPortP is NULL so calling releasePort()
// would crash.
void
closeRemoteSocket(ChannelP chanP, int errnum)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, chanP->chan_fd, 0);
    close( chanP->chan_fd );
    chanP->chan_fd = -1;
    chanP->control_flags |= CNTL_LOST;
    chanP->control_flags &= ~(CNTL_CONNECTED | CNTL_TFULL);
    last_error = chanP->last_err =
        IO_ERR_FLAG | IO_ERR_LOST | (errnum?(IO_ERR_ERRNO | ((errnum & 0377) << 4)):0);
}

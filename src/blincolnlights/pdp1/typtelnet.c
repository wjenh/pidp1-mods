/*
 * Telnet emulation used by various components.
 *
 * 14-Jul-2026 wje first major cleanup pass, it needed it.
 *    Still needs more.
*/
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <poll.h>
#include <pthread.h>

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

/* 20-Jun-2026 wje: dropped the `col` parameter and the wire-bit6-driven color
 * switch.
 * That encoded IOT_3.c's case state (tbb) into bit 6 of every byte and used it to drive the ANSI escape,
 * conflating case and ribbon color which are independent on the real flexowriter.
 * Color now changes only on a Blk/Red code from the table, exactly mirroring how
 * ucase already only changes on a Lcs/Ucs code.
 * The two state machines are now symmetric and independent, as they should be.
 */
static void
putfio(int c, int fd)
{
	const char *s;
	ssize_t wr;

	c = ucase*0100 + (c&077);
	s = fio2uni[c];
	if(s == Lcs) { ucase = 0; return; }
	if(s == Ucs) { ucase = 1; return; }
	if(s == Blk)
    {
		if(color) { color = 0; wr = write(fd, "\033[39;49m", 8); (void)wr; }
		return;
	}
	if(s == Red)
    {
		if(!color) { color = 1; wr = write(fd, "\033[31m", 5); (void)wr; }
		return;
	}
	if(s != XXX) { wr = write(fd, s, strlen(s)); (void)wr; }
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
	(void)wr;	// best-effort; dead peer is caught by the next read()

	/* 20-Jun-2026 wje: was `putfio(color<<6 | s[i], localfd)`.
     * The color<<6 packing was a leftover of the old wire-bit6 color scheme.
     * Putfio() no longer looks at that bit at all, so it's just s[i] now.
     */
	int i;
	for(i = 0; i < n; i++)
    {
		putfio(s[i], localfd);
    }
}


static void
getascii(int c, int fd, int localfd)
{
	/* simulate common combinations
	 * didn't actually use to work so well, but maybe fixed now? */
	if(c == ';')
    {
		getfio(0140, fd, localfd);
		getfio(033, fd, localfd);
	} else if(c == ':')
    {
		getfio(0140, fd, localfd);
		getfio(073, fd, localfd);
	} else
    {
		c = ascii2fio[c];
		if(c < 0)
        {
			return;
        }

		getfio(c, fd, localfd);
	}
}

static int
readiac(int fd)
{
int c;
char cc;
ssize_t rd;

	rd = read(fd, &cc, 1);
	(void)rd;
	c = cc & 0377;
	switch(c)
    {
	case NOP: break;
	case WILL:
		  rd = read(fd, &cc, 1); (void)rd;
		  break;
	case WONT:
		  rd = read(fd, &cc, 1); (void)rd;
		  break;
	case DO:
		  rd = read(fd, &cc, 1); (void)rd;
		  break;
	case DONT:
		  rd = read(fd, &cc, 1); (void)rd;
		  break;
	case IAC:
		  return c;
	default:
		  printf("unknown telnet command %d\n", c);
	}
	return -1;
}

static void
readwrite(int telfd, int typfd)
{
	int n;
	struct pollfd pfd[2];
	char c;
	int ci;		/* audit M3a: holds readiac()'s int-typed result, see below */

	pfd[0].fd = typfd;
	pfd[0].events = POLLIN;
	pfd[1].fd = telfd;
	pfd[1].events = POLLIN;
	while(pfd[0].fd != -1)
    {
		n = poll(pfd, 2, -1);
		if(n < 0){
			perror("error poll");
			return;
		}
		if(n == 0)
			return;
		/* take from pdp, send to telnet */
		if(pfd[0].revents & POLLIN)
        {
			if(n = read(typfd, &c, 1), n <= 0)
				return;
			else
            {
				c &= 0177;
				putfio(c, telfd);
			}
		}
		/* receive over telnet, send to pdp */
		if(pfd[1].revents & POLLIN)
        {
			if(n = read(telfd, &c, 1), n <= 0)
				return;
			else
            {
				if((c&0377) == IAC)
                {
					/* audit M3a: keep readiac()'s result in an int (ci) instead of
					 * assigning it straight into the plain `char c` above. readiac()
					 * returns -1 when it fully consumed a telnet command (nothing to
					 * send), or IAC (255, 0xFF) when the peer sent an escaped IAC --
					 * two consecutive 0xFF bytes representing one literal 0xFF data
					 * byte, per telnet's binary-mode escaping rule. Testing `c < 0`
					 * after assigning 255 into a plain char only behaves correctly if
					 * char happens to be unsigned on this platform (the default on
					 * ARM, but NOT on x86_64, where 255 silently becomes -1) -- an
					 * escaped-IAC data byte would then be indistinguishable from
					 * "nothing to do here" purely by accident of char's signedness.
					 */
					ci = readiac(telfd);
					if(ci < 0)
						continue;	/* telnet command fully consumed, nothing to send */

					/* ci == IAC (255) here: explicit decision, not an accident of
					 * signedness -- a raw 0xFF byte has no representation in the
					 * 7-bit ASCII/Flexowriter code space this driver forwards
					 * (everything below is masked with 0177), so there is nothing
					 * meaningful to send to the typewriter. Drop it. */
					continue;
				}
				if(c < 0)
					continue;
				c &= 0177;	/* needed? */
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
	ssize_t wr;	/* best-effort telnet negotiation write; dead peer caught downstream */

	wr = write(fd, &iac, 1); (void)wr;
	wr = write(fd, &ca, 1); (void)wr;
	if(b >= 0)
    {
        wr = write(fd, &cb, 1); (void)wr;
    }
}

static int typport;
static int typfd;

void*
telthread(void *arg)
{
ssize_t wr;
(void)arg;

	for(;;)
    {
		int telfd = serve1(typport);
		cmd(telfd, WILL, XMITBIN);
		cmd(telfd, DO, XMITBIN);
		cmd(telfd, WILL, ECHO_);
		cmd(telfd, DO, SUPRGA);
		cmd(telfd, WILL, SUPRGA);
		cmd(telfd, WONT, LINEEDIT);
		cmd(telfd, DONT, LINEEDIT);
		/* Reset a fresh connection to default color if a previous connection left
		 * it red. 20-Jun-2026 wje: was `putfio(0160, telfd)`, relying on putfio()'s
		 * old wire-bit6 color logic -- 0160's bit 6 is actually set, so that call
		 * never really reset anything even before this fix. Now that putfio() only
		 * changes color on a genuine Blk/Red table entry (060 octal maps to
		 * XXX/Lcs depending on ucase, never Blk/Red), the reset has to be written
		 * directly instead of routed through putfio(). */
		if(color)
        {
			color = 0;
			wr = write(telfd, "\033[39;49m", 8); (void)wr;
		}
		readwrite(telfd, typfd);
		close(telfd);
	}
}

void
typtelnet(int port, int fd)
{
	pthread_t th;
	typport = port;
	typfd = fd;
	pthread_create(&th, NULL, telthread, NULL);
}

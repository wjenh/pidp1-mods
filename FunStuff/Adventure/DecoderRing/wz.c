/* wz -- Adventure wizard-mode challenge decoder ring.
 *
 * The original PDP-10 installation had a small offline program so a real
 * wizard didn't have to do WIZARD's challenge-response arithmetic by
 * hand.
 * This is that program, ported.
 * Given the 5-letter challenge the game just printed, the wizard's configured MAGNM,
 * and the current time of day, it prints the 5-letter reply that WIZARD will accept.
 *
 * Source of the arithmetic, quirks included: advn2.f4 491-548, the
 * original Fortran WIZARD() function, also ported to am1 in
 * Adventure/adventure.am1 (wzChallenge/wzCheckReply and friends).
 *
 * IMPORTANT: only the *reply* half of the arithmetic is needed here.
 * The game's challenge-generation arithmetic (advn2.f4 513-522) needs the
 * day/time from when WIZARD first printed the challenge, which this
 * standalone tool has no access to, but it doesn't need it.
 * The displayed challenge already encodes everything the reply computation needs.
 * The reply instead depends on a fresh second read of the clock (advn2.f4's second CALL DATIME),
 * taken right when the reply is computed/typed, see f4 527-537.
 *
 * The PDP-1 emulator's clock IOT (IOTs/ChronoLog/IOT_70) reads the
 * host's Linux time() via localtime(), so this tool uses the same,
 * plain wall-clock local time, no timezone/DST handling needed.
 *
 * Usage:
 *   wz [-m MAGNM] [-t HH:MM] [-v] CHALLENGE
 *   wz -s              (run the built-in self-test and exit)
 *
 *   CHALLENGE   the 5-letter challenge word the game just printed
 *               (case-insensitive -- uppercased before use)
 *   -m MAGNM    the WIZCOM magic number (default 11111, the POOF
 *               default -- override if MAINT has changed it)
 *   -t HH:MM    use this local time instead of the real clock (24-hour) --
 *               for testing, or to compute a reply for a specific moment
 *   -v          verbose: also show the challenge, MAGNM, time used, and
 *               how long the reply stays valid
 *   -s          run the built-in self-test (checks this build's
 *               arithmetic against known-correct vectors) and exit
 *
 * Example:
 *   $ wz abcde
 *   BBBBE
 *
 * Note on validity: the reply depends on (hour, minute/10) only.
 * It is stable for the rest of the current ten-minute clock bucket, then
 * changes at the next :00/:10/:20/.../:50 boundary. -v prints that boundary.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Prints the usage summary to stderr. progP is argv[0].
static void
usage(const char *progP)
{
    fprintf(stderr,
        "usage: %s [-m MAGNM] [-t HH:MM] [-v] CHALLENGE\n"
        "       %s -s\n"
        "\n"
        "  CHALLENGE   the 5-letter challenge the game printed (case-insensitive)\n"
        "  -m MAGNM    wizard magic number (default 11111, the POOF default)\n"
        "  -t HH:MM    use this local time instead of the real clock (24-hour)\n"
        "  -v          verbose: show challenge/MAGNM/time-used/valid-until\n"
        "  -s          run the built-in self-test and exit\n",
        progP, progP);
}

// advn2.f4 527-537: given the challenge's 5 letters (as 1-26 alphabet
// positions), the wizard's MAGNM, and the current minutes-past-midnight,
// computes the 5-letter reply WIZARD expects into outP, which must have
// room for 6 bytes (5 letters + NUL).
static void
computeExpectedReply(const int val[5], long magnm, int minutesNow, char outP[6])
{
int x;
int y;
int z;
int diff;
int t2;
long d;                                    /* f4 529 */

    t2 = (minutesNow / 60) * 40 + (minutesNow / 10) * 10; /* f4 528 */
    d = magnm;                            /* f4 529 */

    for( y = 0; y < 5; y++ )
    {
        z = (y + 1) % 5;              /* f4 531: Z=MOD(Y,5)+1, 0-indexed */
        diff = val[y] - val[z];

        if( diff < 0 )
        {
            diff = -diff;                  /* f4 532: IABS(VAL(Y)-VAL(Z)) */
        }

        x = (int)((diff * (d % 10) + (t2 % 10)) % 26) + 1; /* f4 532 */

        t2 /= 10;                          /* f4 533 */
        d /= 10;                           /* f4 534 */

        outP[y] = (char)('A' + x - 1);      /* f4 535's char = '@'+X */
    }
    outP[5] = '\0';
}

// Parses a 24-hour "HH:MM" clock time into minutes past midnight.
// Returns 1 and fills *minutesP on success (0<=HH<=23, 0<=MM<=59);
// returns 0 on a malformed string, leaving *minutesP untouched.
static int
parseHhmm(const char *sP, int *minutesP)
{
    int hh, mm;
    char extra;

    if( sscanf(sP, "%d:%d%c", &hh, &mm, &extra) != 2 )
    {
        return(0);
    }

    if( (hh < 0) || (hh > 23) || (mm < 0) || (mm > 59) )
    {
        return(0);
    }

    *minutesP = hh * 60 + mm;
    return(1);
}

// Uppercases sP in place.
// Returns 1 if sP is exactly 5 alphabetic characters, 0 otherwise; on 0
// the string may have been partly uppercased already.
static int
normalizeChallenge(char *sP)
{
int i;

    if( strlen(sP) != 5 )
    {
        return(0);
    }

    for( i = 0; i < 5; i++ )
    {
        if( !isalpha((unsigned char)sP[i]) )
        {
            return(0);
        }

        sP[i] = (char)toupper((unsigned char)sP[i]);
    }

    return(1);
}

struct SelfTestVector
{
    const char *challenge;
    long magnm;
    int minutes;       /* minutes past midnight */
    const char *expected;
};

// Golden vectors from the game side. A match here means this arithmetic
// agrees with the am1 implementation, without needing the emulator
// running.
static struct SelfTestVector selfTestVectors[] =
{
    { "YZXCV", 11111,  9 * 60 +  7, "BCETD" }, /* 09:07 */
    { "ABCDE", 11111,           0, "BBBBE" }, /* 00:00 */
    { "ZZZZZ", 11111, 23 * 60 + 59, "AFDCA" }, /* 23:59 */
    { "ABCDE", 54321, 12 * 60 + 34, "BFFFU" }, /* 12:34, non-default MAGNM */
};

// Runs every golden vector through computeExpectedReply and prints a
// PASS/FAIL line for each.
// Returns 0 if all vectors passed, 1 if any failed -- the value main
// exits with, so the shell sees a failure as a non-zero status.
static int
runSelfTest(void)
{
int j;
int ok;
int allOk;
size_t i, n;
int val[5];
char got[6];
struct SelfTestVector *vP;

    allOk = 1;
    n = sizeof(selfTestVectors) / sizeof(selfTestVectors[0]);
    for( i = 0; i < n; i++ )
    {
        vP = &selfTestVectors[i];
        for( j = 0; j < 5; j++ )
        {
            val[j] = vP->challenge[j] - 'A' + 1;
        }

        computeExpectedReply(val, vP->magnm, vP->minutes, got);
        ok = (strcmp(got, vP->expected) == 0);
        allOk &= ok;

        printf("[%s] challenge=%s magnm=%ld t=%02d:%02d expected=%s got=%s\n",
            (ok)?"PASS":"FAIL", vP->challenge, vP->magnm,
            vP->minutes / 60, vP->minutes % 60, vP->expected, got);
    }

    printf( (allOk)?"self-test:all %zu vectors passed\n"
                  : "self-test: FAILURES ABOVE\n", n);
    return( !allOk );
}

// Parses the options, reads the clock unless -t overrode it, and prints
// the reply WIZARD will accept for the given challenge.
// Exits 0 on success, 1 if the local clock could not be read, 2 on a
// usage or argument error. With -s, exits with runSelfTest's result.
int
main(int argc, char **argv)
{
int i;
int nextBoundary;
int minutesNow;
int verbose = 0;
int opt;
long magnm;
char challenge[64];
int val[5];
char reply[6];

    magnm = 11111; /* POOF default, advn2.f4 */
    minutesNow = -1;

    while( (opt = getopt(argc, argv, "m:t:vsh")) != -1 )
    {
        switch (opt)
        {
        case 'm':
            magnm = atol(optarg);
            break;
        case 't':
            if( !parseHhmm(optarg, &minutesNow) )
            {
                fprintf(stderr, "wz: -t expects HH:MM (24-hour), got %s\n", optarg);
                return(2);
            }
            break;
        case 'v':
            verbose = 1;
            break;
        case 's':
            return(runSelfTest());
        case 'h':
            usage(argv[0]);
            return(0);
        default:
            usage(argv[0]);
            return(2);
        }
    }

    if( optind >= argc )
    {
        usage(argv[0]);
        return(2);
    }

    if( strlen(argv[optind]) >= sizeof(challenge) )
    {
        fprintf(stderr, "wz: challenge word too long\n");
        return(2);
    }
    strcpy(challenge, argv[optind]);

    if( !normalizeChallenge(challenge) )
    {
        fprintf(stderr, "wz: CHALLENGE must be exactly 5 letters, got %s\n", argv[optind]);
        return(2);
    }

    if( minutesNow < 0 )
    {
        time_t now = time(NULL);
        struct tm *ltP = localtime(&now);

        if( ltP == NULL )
        {
            fprintf(stderr, "wz: could not read the local clock\n");
            return(1);
        }
        minutesNow = ltP->tm_hour * 60 + ltP->tm_min;
    }

    for( i = 0; i < 5; i++ )
    {
        val[i] = challenge[i] - 'A' + 1;
    }

    computeExpectedReply(val, magnm, minutesNow, reply);

    // Next :00/:10/:20/.../:50 boundary, when the reply changes.
    nextBoundary = ((minutesNow / 10) + 1) * 10;
    if( nextBoundary >= 24 * 60 )
    {
        nextBoundary -= 24 * 60;
    }

    if( verbose )
    {
        printf("challenge:  %s\n", challenge);
        printf("magnm:      %ld\n", magnm);
        printf("time used:  %02d:%02d\n", minutesNow / 60, minutesNow % 60);
        printf("reply:      %s\n", reply);
        printf("valid until %02d:%02d local\n", nextBoundary / 60, nextBoundary % 60);
    }
    else
    {
        printf("%s\n", reply);
    }

    return(0);
}

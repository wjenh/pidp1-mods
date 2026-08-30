/* wz -- Adventure wizard-mode challenge decoder ring.
 *
 * The original PDP-10 installation had a small offline program so a real
 * wizard didn't have to do WIZARD()'s challenge-response arithmetic by
 * hand -- this is that program, ported. Given the 5-letter challenge the
 * game just printed, the wizard's configured MAGNM, and the current time
 * of day, it prints the 5-letter reply that WIZARD() will accept.
 *
 * Source of the arithmetic (verbatim, quirks included): advn2.f4 491-548
 * (the original Fortran WIZARD() function), ported to am1 in
 * Adventure/adventure.am1 (wzChallenge/wzCheckReply and friends, see
 * wzMulMod1027 and wcMagnm around line 12500-12820), cross-checked
 * against the host-side Python reference in
 * Claude/SupportCode/testing/adventure_f10c_test.py
 * (compute_val/compute_expected_reply), which was itself live-verified
 * against the real emulator during TASK-F10-WIZARDRY Phase C testing.
 * See Adventure/PendingRework/TASK-F10-WIZARDRY.md's "Companion tool"
 * section for the design mandate.
 *
 * IMPORTANT: only the *reply* half of the arithmetic is needed here. The
 * game's own challenge-generation arithmetic (advn2.f4 513-522) needs the
 * day/time from when WIZARD() first printed the challenge, which this
 * standalone tool has no access to -- but it doesn't need it, either: the
 * displayed challenge already encodes everything the reply computation
 * needs (each letter's 1-26 alphabet position). The reply instead depends
 * on a FRESH second read of the clock (advn2.f4's second CALL DATIME),
 * taken right when the reply is computed/typed -- see f4 527-537.
 *
 * The PDP-1 emulator's clock IOT (IOTs/ChronoLog/IOT_70.c) reads the
 * host's Linux time() via localtime(), so this tool uses the same: plain
 * wall-clock local time, no timezone/DST handling needed.
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
 * Note on validity: the reply depends on (hour, minute/10) only -- it is
 * stable for the rest of the current ten-minute clock bucket, then
 * changes at the next :00/:10/:20/.../:50 boundary. -v prints that
 * boundary.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
usage(const char *prog)
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
        prog, prog);
}

/* advn2.f4 527-537 / adventure.am1's wzCheckReply, faithfully: given the
 * challenge's 5 letters (as 1-26 alphabet positions), the wizard's MAGNM,
 * and the current minutes-past-midnight, compute the 5-letter reply
 * WIZARD() expects. out must have room for 6 bytes (5 letters + NUL). */
static void
compute_expected_reply(const int val[5], long magnm, int minutes_now, char out[6])
{
    int t2 = (minutes_now / 60) * 40 + (minutes_now / 10) * 10; /* f4 528 */
    long d = magnm;                                              /* f4 529 */
    int y;

    for (y = 0; y < 5; y++) {
        int z = (y + 1) % 5;              /* f4 531: Z=MOD(Y,5)+1, 0-indexed */
        int diff = val[y] - val[z];
        int x;

        if (diff < 0)
            diff = -diff;                  /* f4 532: IABS(VAL(Y)-VAL(Z)) */

        x = (int)((diff * (d % 10) + (t2 % 10)) % 26) + 1; /* f4 532 */

        t2 /= 10;                          /* f4 533 */
        d /= 10;                           /* f4 534 */

        out[y] = (char)('A' + x - 1);      /* f4 535's char = '@'+X */
    }
    out[5] = '\0';
}

/* Returns 1 and fills *minutes on success ("HH:MM", 0<=HH<=23, 0<=MM<=59);
 * returns 0 on a malformed string. */
static int
parse_hhmm(const char *s, int *minutes)
{
    int hh, mm;
    char extra;

    if (sscanf(s, "%d:%d%c", &hh, &mm, &extra) != 2)
        return 0;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
        return 0;

    *minutes = hh * 60 + mm;
    return 1;
}

/* Uppercases in place; returns 1 if s is exactly 5 alphabetic characters,
 * 0 otherwise. */
static int
normalize_challenge(char *s)
{
    int i;

    if (strlen(s) != 5)
        return 0;

    for (i = 0; i < 5; i++) {
        if (!isalpha((unsigned char)s[i]))
            return 0;
        s[i] = (char)toupper((unsigned char)s[i]);
    }
    return 1;
}

struct self_test_vector {
    const char *challenge;
    long magnm;
    int minutes;       /* minutes past midnight */
    const char *expected;
};

/* Golden vectors from the host-side Python reference
 * (Claude/SupportCode/testing/adventure_f10c_test.py's
 * compute_expected_reply), which was itself live-verified against the
 * real emulator's wzWizard during TASK-F10-WIZARDRY Phase C testing.
 * A match here means this C port's arithmetic is byte-identical to the
 * shipped am1 implementation without needing the emulator running. */
static const struct self_test_vector self_test_vectors[] = {
    { "YZXCV", 11111,  9 * 60 +  7, "BCETD" }, /* 09:07 */
    { "ABCDE", 11111,           0, "BBBBE" }, /* 00:00 */
    { "ZZZZZ", 11111, 23 * 60 + 59, "AFDCA" }, /* 23:59 */
    { "ABCDE", 54321, 12 * 60 + 34, "BFFFU" }, /* 12:34, non-default MAGNM */
};

static int
run_self_test(void)
{
    size_t i, n = sizeof(self_test_vectors) / sizeof(self_test_vectors[0]);
    int all_ok = 1;

    for (i = 0; i < n; i++) {
        const struct self_test_vector *v = &self_test_vectors[i];
        int val[5];
        char got[6];
        int j;
        int ok;

        for (j = 0; j < 5; j++)
            val[j] = v->challenge[j] - 'A' + 1;

        compute_expected_reply(val, v->magnm, v->minutes, got);
        ok = (strcmp(got, v->expected) == 0);
        all_ok &= ok;

        printf("[%s] challenge=%s magnm=%ld t=%02d:%02d expected=%s got=%s\n",
               ok ? "PASS" : "FAIL", v->challenge, v->magnm,
               v->minutes / 60, v->minutes % 60, v->expected, got);
    }

    printf(all_ok ? "self-test: all %zu vectors passed\n"
                  : "self-test: FAILURES ABOVE\n", n);
    return all_ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
    long magnm = 11111; /* POOF default, advn2.f4 */
    int minutes_now = -1;
    int verbose = 0;
    int opt;
    char challenge[64];
    int val[5];
    char reply[6];
    int i;
    int next_boundary;

    while ((opt = getopt(argc, argv, "m:t:vsh")) != -1) {
        switch (opt) {
        case 'm':
            magnm = atol(optarg);
            break;
        case 't':
            if (!parse_hhmm(optarg, &minutes_now)) {
                fprintf(stderr, "wz: -t expects HH:MM (24-hour), got %s\n", optarg);
                return 2;
            }
            break;
        case 'v':
            verbose = 1;
            break;
        case 's':
            return run_self_test();
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }

    if (strlen(argv[optind]) >= sizeof(challenge)) {
        fprintf(stderr, "wz: challenge word too long\n");
        return 2;
    }
    strcpy(challenge, argv[optind]);

    if (!normalize_challenge(challenge)) {
        fprintf(stderr, "wz: CHALLENGE must be exactly 5 letters, got %s\n", argv[optind]);
        return 2;
    }

    if (minutes_now < 0) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);

        if (lt == NULL) {
            fprintf(stderr, "wz: could not read the local clock\n");
            return 1;
        }
        minutes_now = lt->tm_hour * 60 + lt->tm_min;
    }

    for (i = 0; i < 5; i++)
        val[i] = challenge[i] - 'A' + 1;

    compute_expected_reply(val, magnm, minutes_now, reply);

    /* Next :00/:10/:20/.../:50 boundary, when the reply changes. */
    next_boundary = ((minutes_now / 10) + 1) * 10;
    if (next_boundary >= 24 * 60)
        next_boundary -= 24 * 60;

    if (verbose) {
        printf("challenge:  %s\n", challenge);
        printf("magnm:      %ld\n", magnm);
        printf("time used:  %02d:%02d\n", minutes_now / 60, minutes_now % 60);
        printf("reply:      %s\n", reply);
        printf("valid until %02d:%02d local\n", next_boundary / 60, next_boundary % 60);
    } else {
        printf("%s\n", reply);
    }

    return 0;
}

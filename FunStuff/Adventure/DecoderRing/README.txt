wz -- Adventure wizard-mode challenge decoder ring.

The original PDP-10 install had an offline program to compute the
correct reply to WIZARD()'s clock-seeded challenge, so a real wizard
didn't have to do the arithmetic by hand. This is that program.

Usage:
    wz [-m MAGNM] [-t HH:MM] [-v] CHALLENGE
    wz -s

    CHALLENGE   the 5-letter challenge the game just printed
                (case-insensitive)
    -m MAGNM    wizard magic number (default 11111, the POOF default --
                override if MAINT has changed it)
    -t HH:MM    use this local time instead of the real clock (24-hour)
    -v          verbose: show challenge/MAGNM/time-used/valid-until
    -s          run the built-in self-test and exit

Example:
    $ wz abcde
    BBBBE

See the header comment in wz.c for the arithmetic and its sources
(advn2.f4 491-548, adventure.am1's wzChallenge/wzCheckReply,
Claude/SupportCode/testing/adventure_f10c_test.py).

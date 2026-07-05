/*
 * Shared-memory layout for the pidp-1 front panel (switches and lamp state),
 * used by the emulator (pdp1), the hardware panel driver (panel_pidp1 and newpanel), and
 * the browser-based panel emulator (vpanel_pdp1).
 *
 * wje 14-Jun-26 - add pwmcount[][] for emulator-side lamp duty-cycle tallies
 * wje 14-Jun-26 - widen pwmcount[][] to u16 to give the panel driver more
 *                 headroom for longer integration windows and scheduling
 *                 delays without saturating
 * wje 14-Jun-26 - add cyclecount, a monotonically-incrementing count of
 *                 emulated cycles, so the panel driver can compute the
 *                 actual number of cycles elapsed between its samples
 *                 instead of inferring it from wall-clock time
 * wje 4-Jul-25 - just formatting cleanup, no functional change
 */
enum {
    // sw0
    SW_EXTEND = 0400000,
    SW_POWER = 0200000,

    // sw2
    SW_SSTEP = 0400000,
    SW_SINST = 0200000,
    // SS: 100000 - 002000
    KEY_START = 0001000,
    KEY_START_UP = 0000400,
    KEY_STOP = 0000200,
    KEY_CONT = 0000100,
    KEY_EXAM = 0000040,
    KEY_DEP = 0000020,
    KEY_READIN = 0000010,
    KEY_READER = 0000004,
    KEY_READER_UP = 0000002,
    KEY_FEED = 0000001,

    L5_RUN = 0400000,
    L5_CYC = 0200000,
    L5_DF1 = 0100000,
    L5_HSC = 0040000,
    L5_BC1 = 0020000,
    L5_BC2 = 0010000,
    L5_OV1 = 0004000,
    L5_RIM = 0002000,
    L5_SBM = 0001000,
    L5_EXD = 0000400,
    L5_IOH = 0000200,
    L5_IOC = 0000100,
    L5_IOS = 0000040,
    L5_PWR = 0000004,
    L5_SSTEP = 0000002,
    L5_SINST = 0000001,
    // L6:
    // IR: 400000-020000
    // SS: 004000-000100
    // PF: 000040-000001
};

typedef struct Panel Panel;
struct Panel
{
    int sw0;
    int sw1;
    int sw2;
    int sw3;        // controllers
    int lights0;
    int lights1;
    int lights2;
    int lights3;
    int lights4;
    int lights5;
    int lights6;
    // IO panel
    int lights7;
    int lights8;
    int lights9;

    // just for convenience
    int psw2;

    // Per-lamp "on" tallies, added to support a lower-overhead lamp PWM
    // scheme. updatelights() in pdp1/panel1.c increments pwmcount[row][col]
    // once per emulated cycle for each lamp bit that is set in that
    // cycle's lights snapshot.
    // Indexed the same as panel_pidp1's PanelLamps.lamps[10][18],
    // rows 0-6 correspond to lights0-lights6 (main panel), rows 7-9 to
    // lights7-lights9 (I/O panel).
    //
    // The panel driver is expected to periodically read and then reset
    // these counters and scale the result into a PWM "on" duration
    // instead of polling lights0-lights9 at a high sample rate as panel_pidp1 does.
    //
    // Existing lights0-lights9 fields are unchanged and continue to be
    // updated as before for the browser-based panel (vpanel_pdp1), which
    // does its own sampling and does not use pwmcount[][].
    //
    // Not synchronized against concurrent updates from pdp1; a reader may
    // occasionally race an increment by +/-1, which is negligible given
    // the intended read interval.
    u16 pwmcount[10][18];

    // Monotonically-incrementing count of emulated cycles, incremented by
    // 1 in updatelights() (panel1.c) every time it's called once per
    // emulated cycle, whether the cpu is running or halted. The panel
    // driver reads this once per sampling iteration and computes the
    // delta since its last reading to get the true number of cycles that
    // occurred during that interval, rather than assuming a fixed
    // 5us/cycle rate based on wall-clock time.
    // Wraps silently, but it's a u64, so in practice never.
    u64 cyclecount;
};

// Defines shared data between the IOT layer and the emulator
#include <pthread.h>
#include <stdatomic.h>

/*
 * The logic around emuCommandSet() and emuResponseSet() is critical to avoid jitter caused by
 * the linux scheduler and to avoid an issue on some ARM cortex cpus.
 * The IOT side sets the command word BEFORE calling this macro.
 * A release fence orders the prior write to ctlP->command before the plain
 * volatile store to commandSent, guaranteeing that the command value is fully
 * visible before any thread observes commandSent == true.
 * This matters on weakly-ordered ARM cpus (Pi 4).
 *
 * commandSent is volatile,specifically not _Atomic.
 * The release fence here pairs with the acquire fence in get340Command's taken branch.
 * The per-iteration load of commandSent in get340Command is a bare volatile
 * read with zero barrier cost; the acquire fence executes only when the flag
 * reads true.
 * This is the standard ARM/GCC idiom chosen deliberately over atomics for timing fidelity.
 */
#define emuCommandSet(ctlP) \
    (atomic_thread_fence(memory_order_release), \
     (ctlP)->commandSent = true, \
     emuWakeup(ctlP))

/*
 * The emulator side sets the response word BEFORE calling this macro.
 * The same release-fence logic as emuCommandSet applies.
 */
#define emuResponseSet(ctlP) \
    (atomic_thread_fence(memory_order_release), \
     (ctlP)->responseSent = true)

#define EMU_CMD_NONE 0      // no command available
#define EMU_CMD_EXIT 1      // terminate the emulator thread
#define EMU_CMD_RUN 2       // start interpreting, runs until a STOP condition, then sets EMU_DONE
#define EMU_CMD_STOP 3      // stop interpreting, wait for a RUN
#define EMU_CMD_RESUME 4    // continue from where we left off
#define EMU_CMD_PAUSE 5     // pause interpreter

#define EMU_CMDFLAG_CLEAR 0100  // special flag to combine CLEAR with RUN or RESUME

#define EMU_RESPONSE_NONE 0  // no response available
#define EMU_RESPONSE_DONE 1  // program complete, status is updated and it waits for a RUN or EXIT
#define EMU_RESPONSE_FAIL 2  // some fatal condition occurred

// flags that can be set for skips, etc.
#define FLAG_VEDGE 01
#define FLAG_HEDGE 02
#define FLAG_STOP 04
#define FLAG_LP 010

typedef struct {
    PDP1P pdp1P;                // pdp-1 access
    int address;                // core mem address of program instructions
    int command;                // command word -- written before commandSent set
    int response;               // response word -- written before responseSent set
    volatile bool commandSent;  // IOT->emulator: new command is ready, must be a volatile
    volatile bool responseSent; // emulator->IOT: new response is ready, must be a volatile
} EmuControl, *EmuControlP;

EmuControlP getEmuControlP(void);
void emuInitialize(PDP1P pdp1P);
bool emuIsInitialized(void);
bool emuIsPaused(void);
bool emuIsRunning(void);
void emuWakeup(EmuControlP ctlP);
int emuGetAddress(void);
int emuGetFlags(void);
void emuClearFlags(void);
void emuGetXY(int *xP, int *yP);

int get340Command(EmuControlP ctlP);
int get340Response(EmuControlP ctlP);

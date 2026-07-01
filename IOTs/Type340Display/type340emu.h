// Defines shared data between the IOT layer and the emulator
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

/*
 * emuCommandSet -- IOT side sets the command word BEFORE calling this macro.
 * The atomic release store on commandSent acts as a write barrier: it guarantees
 * that the prior write to ctlP->command is globally visible before any thread
 * observes commandSent == true.  This matters on weakly-ordered ARM (Pi 4).
 * emuWakeup() follows so that a sleeping emulator thread is unblocked.
 * Fire-and-forget: dla clears flags host-side before calling this macro, so
 * no cross-thread ack is needed for correct dss reads after dla.
 */
#define emuCommandSet(ctlP) \
    (atomic_store_explicit(&(ctlP)->commandSent, true, memory_order_release), \
     emuWakeup(ctlP))

/*
 * emuResponseSet -- emulator side sets the response word BEFORE calling this
 * macro.  Same release-barrier logic as emuCommandSet, ensuring ctlP->response
 * is visible before responseSent is seen as true by the IOT side.
 */
#define emuResponseSet(ctlP) \
    atomic_store_explicit(&(ctlP)->responseSent, true, memory_order_release)

#define EMU_CMD_NONE 0      // no command available
#define EMU_CMD_EXIT 1      // terminate the emulator thread
#define EMU_CMD_RUN 2       // start interpreting, runs until a STOP condition, then sets EMU_DONE
#define EMU_CMD_STOP 3      // stop interpreting, wait for a RUN
#define EMU_CMD_RESUME 4    // continue from where we left off
#define EMU_CMD_PAUSE 5     // pause interpreter
#define EMU_CMD_UPDATE 6    // rescan the config file

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
    PDP1P pdp1P;            /* pdp-1 access                                  */
    int address;            /* core mem address of program instructions       */
    int command;            /* command word -- written before commandSent set */
    int response;           /* response word -- written before responseSent set */
    _Atomic bool commandSent;   /* IOT→emulator: new command is ready        */
    _Atomic bool responseSent;  /* emulator→IOT: new response is ready       */
    /*
     * NOTE: The semaphore used for idle-wait lives as a file-static in
     * type340emu.c (static sem_t waitSemaphore).  No semaphore field is needed
     * here; the former sem_t waitSemaphore member was vestigial and has been
     * removed to avoid confusion.
     */
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

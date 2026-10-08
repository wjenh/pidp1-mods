// Defines shared data between the IOT layer and the emulator
// 8-Oct-2026 Claude - commands go to the 340 thread through an in-order queue instead of one slot.
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

// Commands wait here for the 340 thread, in the order the IOTs issued them. A power of 2, so the
// free-running head and tail counters wrap correctly and index by masking.
#define EMU_QUEUE_SIZE 16
#define EMU_QUEUE_MASK (EMU_QUEUE_SIZE - 1)

/*
 * emuResponseSet -- emulator side sets the response word BEFORE calling this
 * macro.  The release store ensures ctlP->response
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

// One queued command. The address is the dla's start address for EMU_CMD_RUN, unused otherwise.
typedef struct {
    int command;
    int address;
} EmuCommand, *EmuCommandP;

// The queue has one producer, the emulator's main thread (the IOTs, iotStop() and iotUpdate()), and
// one consumer, the 340 thread, so it needs no lock: each side stores only its own counter.
// head - tail is the number of entries waiting.
typedef struct {
    PDP1P pdp1P;            /* pdp-1 access                                  */
    EmuCommand queue[EMU_QUEUE_SIZE];
    _Atomic unsigned int head;  /* entries ever queued; stored only by the IOT side  */
    _Atomic unsigned int tail;  /* entries ever taken; stored only by the 340 thread */
    int response;           /* response word -- written before responseSent set */
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
void emuGetXY(int *dispNoP, int *xP, int *yP);

void emuCommandSet(EmuControlP ctlP, int command, int address);
int get340Command(EmuControlP ctlP, int *addressP);
int get340Response(EmuControlP ctlP);

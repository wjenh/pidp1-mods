// Defines shared data between the IOT layer and the emulator
#include <pthread.h>
#include <semaphore.h>

#define emuCommandSet(ctlP) (ctlP->commandSent = true, emuWakeup(ctlP))
#define emuResponseSet(ctlP) (ctlP->responseSent = true);

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
    PDP1P pdp1P;    // pdp-1 access

    sem_t waitSemaphore;    // how we wait when idle
    int address;    // core mem address of program instructions
    int command;    // from above
    int response;   // from above
    bool commandSent;       // command sent from IOT
    bool responseSent;      // response sent to IOT
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

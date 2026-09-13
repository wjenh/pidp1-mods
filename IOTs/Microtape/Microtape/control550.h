/*
 * Definitions for the tape implementation.
 */
#ifndef CONTROL550_H
#define CONTROL550_H

#include <stdint.h>
#include <stdbool.h>

#include "transport555.h"

// ---- IOT sub-device codes (instruction bits 7-11) on device 01 --------------------------------

#define MT_SUB_MSE          3               // 720301 select unit
#define MT_SUB_MLC          4               // 720401 load control
#define MT_SUB_MRD          5               // 720501 read buffer
#define MT_SUB_MWR          6               // 720601 write buffer
#define MT_SUB_MRS          7               // 720701 read status

// ---- LOAD CONTROL word, IO bits 12-17 (manual p. 2-5) -----------------------------------------

#define MT_CTL_GO           0000040         // bit 12
#define MT_CTL_REV          0000020         // bit 13
#define MT_CTL_MODE         0000007         // bits 15-17
#define MT_MODE_MOVE        0
#define MT_MODE_SEARCH      1
#define MT_MODE_READ        2
#define MT_MODE_WRITE       3               // 4-6 act as move (5 and 6, through block ends, not emulated)
#define MT_MODE_ERASE       7               // manual: write mark track. Here: erase the reel, then move

// ---- SELECT word: unit number in IO bits 2-5 ---------------------------------------------------

#define MT_SEL_SHIFT        12
#define MT_SEL_MASK         017             // 1-7 = units 1-7, 010 = unit 8, others = none

// ---- READ STATUS word, IO bits 0-8 (manual p. 2-6) ---------------------------------------------

#define MT_ST_DF            0400000         // bit 0 data flag
#define MT_ST_BEF           0200000         // bit 1 block end flag
#define MT_ST_ERF           0100000         // bit 2 error flag
#define MT_ST_END           0040000         // bit 3 end of tape
#define MT_ST_MISS          0020000         // bit 4 timing error
#define MT_ST_REV           0010000         // bit 5 reverse
#define MT_ST_GO            0004000         // bit 6 go
#define MT_ST_MTE           0002000         // bit 7 mark track error (never set: the mark track is perfect)
#define MT_ST_UNABLE        0001000         // bit 8 tape unable

// ---- Search words: mark code in bits 0-5, block number in bits 6-17 (DECUS 1963 p. 10, Fig. 6) --

#define MT_MARK_FWD         0260000         // block mark code 26 as read forward
#define MT_MARK_REV         0450000         // its complement obverse, 45, as read in reverse

#define MT_SELECT_DIP_NS    34000000LL      // selection delay, 34 ms: DEC's field-service memo (Vonada,
                                            // June 1964) has the program deselect for 34 ms when changing
                                            // drives; modeled as a control-imposed period with no flags
#define MT_DEFAULT_SBS      2               // default sequence break channel

// Called whenever the control raises the data, block end or error flag, all of which
// request a program break (manual p. 2-6).
typedef void (*Mt550BreakFnP)(void *ctxP);

typedef struct
{
    Mt555Unit units[MT_UNITS + 1];  // units[1] .. units[8]; units[0] is never used

    int selUnit;            // selected unit 1-8, 0 = none
    int mode;               // mode bits of the last accepted LOAD CONTROL, 0-7
    uint32_t buffer;        // the in-out buffer (MMIOB), 18 bits

    bool df;                // data flag
    bool bef;               // block end flag
    bool erf;               // error flag
    bool end;               // END: the tape ran into the end zone ahead
    bool miss;              // MISS: a flag came due while the last one was still up
    bool mte;               // mark track error; kept for the status word, never set
    bool unable;            // a LOAD CONTROL was refused, or the selected tape left its reel

    bool selDip;            // selection delay in progress: no flags until selDipEnd
    uint64_t selDipEnd;

    // Write pipeline (see onBoundary() in control550.c).
    bool wren;                  // WRITE ENABLE: set by an mlc with go and write mode; cleared by any
                                // other mlc, by any error, and by a halt (H-550 pp. 2-19, 2-32)
    bool writeReqOutstanding;   // a word has been asked for and not yet taken to the tape
    bool writeSkip;             // read -> write inside a block: the next boundary is lost
    bool writeDFPending;        // write was entered below speed: raise its DF on reaching it
    bool ctlHeld;               // an mlc given at the last data word waits for the checksum (D256)
    uint32_t heldCtl;           // its IO word

    uint64_t lastTime;      // time the control was last serviced to
    uint64_t nextEvent;     // earliest time anything can happen; service is a no-op before it

    Mt550BreakFnP breakFnP;
    void *breakCtxP;
    unsigned long breakCount;   // break requests so far (host tests check this)
} Mt550, *Mt550P;

void mt550Init(Mt550P cP, Mt550BreakFnP breakFnP, void *breakCtxP);
void mt550Service(Mt550P cP, uint64_t now);

// The five IOTs. The ones that return a value return the new IO register contents.
void mt550Select(Mt550P cP, uint64_t now, uint32_t io);
void mt550LoadControl(Mt550P cP, uint64_t now, uint32_t io);
uint32_t mt550ReadBuffer(Mt550P cP, uint64_t now);
void mt550WriteBuffer(Mt550P cP, uint64_t now, uint32_t io);
uint32_t mt550Status(Mt550P cP, uint64_t now);

// Housekeeping for the plugin.
Mt555UnitP mt550Unit(Mt550P cP, int unit);
void mt550UnitRemounted(Mt550P cP, int unit, uint64_t now);
void mt550FlushAll(Mt550P cP);
void mt550AllHalt(Mt550P cP, uint64_t now);

#endif

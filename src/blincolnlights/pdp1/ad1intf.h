// This contains the interface defines for the ad1 debugger.
// It is enabled by defining USEAD1 in pdp1.h.
#ifndef AD1NTF_H
#define AD1NTF_H

// THe number of breakpoints that can be set at one time
#define AD1_NUM_BREAKPOINTS 8
// THe number of watches that can be set at one time
#define AD1_NUM_WATCHES 8

#define AD1_START_FLAG 0x1
#define AD1_SINGLE_FLAG 0x2
#define AD1_CONTINUE_FLAG 0x4
#define AD1_BRKENABLED_FLAG 0x10
#define AD1_WATCHENABLED_FLAG 0x20
#define AD1_STOP_FLAG 0x40

#define AD1_START(p) (p->ad1flags & AD1_START_FLAG)
#define AD1_STOP(p) (p->ad1flags & AD1_STOP_FLAG)
#define AD1_SINGLE(p) (p->ad1flags & AD1_SINGLE_FLAG)
#define AD1_CONTINUE(p) (p->ad1flags & AD1_CONTINUE_FLAG)
#define AD1_SET_START(p) (p->ad1flags |= AD1_START_FLAG)
#define AD1_SET_STOP(p) (p->ad1flags |= AD1_STOP_FLAG)
#define AD1_SET_SINGLE(p) (p->ad1flags |= AD1_SINGLE_FLAG)
#define AD1_SET_CONTINUE(p) (p->ad1flags |= AD1_CONTINUE_FLAG)
#define AD1_CLEAR_START(p) (p->ad1flags &= ~AD1_START_FLAG)
#define AD1_CLEAR_STOP(p) (p->ad1flags &= ~AD1_STOP_FLAG)
#define AD1_CLEAR_SINGLE(p) (p->ad1flags &= ~AD1_SINGLE_FLAG)
#define AD1_CLEAR_CONTINUE(p) (p->ad1flags &= ~AD1_CONTINUE_FLAG)

#define AD1_ENABLE_BREAKPOINTS(p) (p->ad1flags |= AD1_BRKENABLED_FLAG)
#define AD1_DISABLE_BREAKPOINTS(p) (p->ad1flags &= ~AD1_BRKENABLED_FLAG)
#define AD1_BREAKPOINTS_ENABLED(p) (p->ad1flags & AD1_BRKENABLED_FLAG)
#define AD1_BREAKPOINT_HIT(p) (p->ad1brkHit)
#define AD1_SET_BREAKPOINT_HIT(p) (p->ad1brkHit = true)
#define AD1_CLEAR_BREAKPOINT_HIT(p) (p->ad1brkHit = false)

#define AD1_ENABLE_WATCHES(p) (p->ad1flags |= AD1_WATCHENABLED_FLAG)
#define AD1_DISABLE_WATCHES(p) (p->ad1flags &= ~AD1_WATCHENABLED_FLAG)
#define AD1_WATCHES_ENABLED(p) (p->ad1flags & AD1_WATCHENABLED_FLAG)
#define AD1_WATCH_HIT(p) (p->ad1watchHit)
#define AD1_SET_WATCH_HIT(p) (p->ad1watchHit = true)
#define AD1_CLEAR_WATCH_HIT(p) (p->ad1watchHit = false)

// All the context we need for a breakpoint
typedef struct {
    bool isSet;
    bool isEnabled;
    int number;     // just a convenience
    uint32_t address;
    int count;      // how many times it has to hit before it fires
    int curCount;   // curent number of times it has been hit
    } Breakpoint, *BreakpointP;

// And for watches
typedef struct {
    bool isSet;
    bool isEnabled;
    bool onAny;
    int number;     // just a convenience
    uint32_t address;
    int value;      // value to fire on based on, ignored if onAny is set
    int lastVal;    // for detecting an any change
    } Watch, *WatchP;

#endif

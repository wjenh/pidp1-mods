/**
 * This include defines data structures for the emulation of the Type 19 High Speed Channel Control.
 */

// Statuses that can be returned

#define HSC_OK      0
#define HSC_ERR     -1
#define HSC_BUSY    1
#define HSC_DONE    2

// These are bitflags in the mode field
#define HSC_MODE_FROMMEM       001
#define HSC_MODE_TOMEM         002
#define HSC_MODE_IMMEDIATE     004
#define HSC_MODE_STEAL         010

typedef struct _HSC_ {
    int status;         // one of the HSC_status codes above
    int mode;           // current operation mode, from, to, or both
    int count;          // number of words to transfer
    int memBank;        // memory bank to transfer to/from, 0-15 dec
    int memAddr;        // address offset in memory, offset in bank, 0-4095
    Word *fromBufP;     // should be 4k unless you're sure your count won't exceed the size
    Word *toBufP;       // ditto
    } HSC_Control, *HSC_ControlP;

// called from the emulator run loop
int processHSChannels(PDP1 *pdp1P);

// user methods
int HSC_request_channel(
    PDP1 *pdp1P,        // emulator context
    int chan,           // channel,1-3, channel 1 being highest priority
    int mode,           // HSC_MODE_FROMMEM, _TOMEM, etc. (or'd together)
    int count,          // number of words to transfer, 0-4096
    int memBank,        // memory bank, 0-15
    int memAddr,        // address in bank, 0-4095
    Word *toBuffer,     // the user buffer to copy to memory, must be at least count size
    Word *fromBuffer);  // the user buffer to copy memory into, must be at least count size

int HSC_get_status(int chan);   // returns one of the HSC statuses

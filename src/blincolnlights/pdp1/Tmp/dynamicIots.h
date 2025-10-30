// Include for the dynamic IOT processor

// What's called from pdp1.c
int dynamicIotProcessor(PDP1 *pdpP, int device, int pulse, int completion);
void dynamicIotProcessorStop(void);

// What a loadable IOT handler implements, PDP1 state, pulse hi/low, completion pulse wanted
// The IOT handler implements a function 'int iotHandler(PDP1 *pdp1P, int pulse, int completion)'.
// The handler function will be called twice for each IOT executed, once for start pulse going high,
// again with the start pulse goes low.
typedef int (*IotHandlerP)(PDP1 *, int, int);

// If implemented, will be called when the emulator goes into halt
// The IOT handler implements a function 'void iotStop()'.
typedef void (*IotStopP)(void);

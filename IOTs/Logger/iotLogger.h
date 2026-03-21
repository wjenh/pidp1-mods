/**
 * Include file for iotLogger, also include it in your code.
 */

#ifdef DOLOGGING
void _iotLog(char *fmt, ...);
void _iotCondLog(int enable, char *fmt, ...);
void _iotCloseLog(void);

#define iotLog(fmt, ...) _iotLog(fmt  __VA_OPT__(,)__VA_ARGS__)
#define iotCondLog(enable, fmt, ...) _iotCondLog(enable, fmt __VA_OPT__(,)__VA_ARGS__)
#define iotCloseLog() _iotCloseLog()
#else
#define iotLog(fmt, ...)
#define iotCondLog(enable, fmt, ...)
#define iotCloseLog()
#endif

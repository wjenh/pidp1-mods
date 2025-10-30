/**
 * Include file for logger, also include it in your code.
 */

#ifdef DOLOGGING
void _logger(char *fmt, ...);
void _closeLog(void);

#define logger(fmt, ...) _logger(fmt __VA_OPT__(,)__VA_ARGS__)
#define closeLog() _closeLog()
#else
#define logger(fmt, ...)
#define closeLog()
#endif

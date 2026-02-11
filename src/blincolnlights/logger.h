/**
 * Include file for logger, also include it in your code.
 */

#ifndef LOGGER_H
#define LOGGER_H
#ifdef DOLOGGING
void _logger(int enable, char *fmt, ...);
void _closeLog(void);

#define logger(enable, fmt, ...) _logger(enable, fmt __VA_OPT__(,)__VA_ARGS__)
#define closeLog() _closeLog()
#else
#define logger(enable, fmt, ...)
#define closeLog()
#endif
#endif

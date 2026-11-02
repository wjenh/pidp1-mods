// The terminal: the telnet connection, the line reader and the commands typed
// after /.

#ifndef TERM_H
#define TERM_H

// Serves the telnet port for good, a client at a time, on this thread.
void termServe(int port);

#endif

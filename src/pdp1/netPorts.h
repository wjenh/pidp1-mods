// The emulator's network ports (netPorts.c).
#ifndef NETPORTS_H
#define NETPORTS_H

// Thread entry: serves the command, tape and display ports; arg is the PDP1 the command port
// works on.
void *netthread(void *arg);

#endif

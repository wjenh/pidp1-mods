// Process status for pdp1central: which of the emulator's programs are running, and whether a
// localhost port accepts a connection.
// Processes are matched by their exact name in /proc/<pid>/comm, never by a pattern: a pattern
// such as 'pdp1$' also matches vpanel_pdp1. The kernel keeps only the first 15 characters of a
// name there, so the name asked for is cut to 15 before comparing.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core.h"

#define COMM_LEN 15             // TASK_COMM_LEN less its NUL
#define PORT_PROBE_MS 200

// Count the processes whose name is nameP.
// Returns the number running, 0 if none.
int
procCount(const char *nameP)
{
DIR *dirP;
struct dirent *entP;
FILE *fP;
char path[sizeof(entP->d_name) + 16];
char comm[64];
size_t len;
int count;

    if( !(dirP = opendir("/proc")) )
    {
        return(0);
    }

    count = 0;
    while( (entP = readdir(dirP)) )
    {
        if( (entP->d_name[0] < '0') || (entP->d_name[0] > '9') )
        {
            continue;
        }

        snprintf(path, sizeof(path), "/proc/%s/comm", entP->d_name);
        if( !(fP = fopen(path, "r")) )
        {
            continue;           // it ended while we looked
        }

        if( fgets(comm, sizeof(comm), fP) )
        {
            len = strcspn(comm, "\n");
            comm[len] = '\0';
            if( !strncmp(comm, nameP, COMM_LEN) && ((strlen(nameP) >= COMM_LEN) ? (len == COMM_LEN) :
                (len == strlen(nameP))) )
            {
                count++;
            }
        }

        fclose(fP);
    }

    closedir(dirP);
    return(count);
}

// Whether 127.0.0.1:port accepts a connection within 200 ms.
// The connection is closed at once without sending anything.
bool
portAnswers(int port)
{
struct sockaddr_in addr;
struct pollfd pfd;
socklen_t errLen;
int fd, err;
bool answered;

    if( (fd = socket(AF_INET, (SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC), 0)) < 0 )
    {
        return(false);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    answered = false;
    if( !connect(fd, (struct sockaddr *)&addr, sizeof(addr)) )
    {
        answered = true;
    }
    else if( errno == EINPROGRESS )
    {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if( poll(&pfd, 1, PORT_PROBE_MS) == 1 )
        {
            errLen = sizeof(err);
            answered = (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) && (err == 0));
        }
    }

    close(fd);
    return(answered);
}

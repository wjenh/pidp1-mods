// Runs one child program at a time without blocking the event loop: pdp1control.sh for start,
// stop, restart and the reloads, and the file dialogs.
// Also starts the companions detached (launchDetached), outside that one slot.
// The child gets its own session (setsid), so the emulator the script starts is not in
// pdp1central's session and outlives it.
// Its stdin is /dev/null, and its stdout and stderr go to a non-blocking pipe that childPoll() drains.
// The child is judged finished when waitpid() reports it, not at the pipe's end: a program the
// script starts in the background (screen, nohup) can hold the pipe open indefinitely.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core.h"

static pid_t childPid;          // 0 when no child is running
static int childFd = -1;        // the read end of the output pipe
static int lastStatus;

#define LAUNCH_WAIT_MS 2000     // the longest launchDetached waits to learn whether exec worked

// In a new child, put back the signals' defaults and an empty mask: an ignored or blocked signal
// stays so across exec, and a program started here must not inherit what the app or SDL did.
static void
defaultSignals(void)
{
sigset_t none;
int sig;

    for( sig = 1; sig < NSIG; sig++ )
    {
        if( (sig != SIGKILL) && (sig != SIGSTOP) )
        {
            signal(sig, SIG_DFL);
        }
    }

    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

// Start argvP[0] with arguments argvP, unless a child is still running.
// Returns false if one is, or if the child could not be started.
bool
childStart(char *const argvP[])
{
int pipeFds[2];
int nullFd;
pid_t pid;

    if( childPid || (childFd >= 0) )
    {
        return(false);
    }

    if( pipe2(pipeFds, O_CLOEXEC) )
    {
        return(false);
    }

    if( (pid = fork()) < 0 )
    {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return(false);
    }

    if( pid == 0 )
    {
        // The emulator the script starts must not inherit the app's signals either.
        defaultSignals();
        setsid();
        if( (nullFd = open("/dev/null", O_RDONLY)) >= 0 )
        {
            dup2(nullFd, 0);
        }

        dup2(pipeFds[1], 1);
        dup2(pipeFds[1], 2);
        execv(argvP[0], argvP);
        dprintf(2, "cannot run %s: %s\n", argvP[0], strerror(errno));
        _exit(127);
    }

    close(pipeFds[1]);
    fcntl(pipeFds[0], F_SETFL, (fcntl(pipeFds[0], F_GETFL) | O_NONBLOCK));
    childFd = pipeFds[0];
    childPid = pid;
    return(true);
}

// Read whatever output is waiting, without blocking, into outP (always NUL-terminated).
// Returns the number of bytes read.
static size_t
drainOutput(char *outP, size_t outLen)
{
size_t got;
ssize_t n;

    got = 0;
    while( (childFd >= 0) && (got < (outLen - 1)) )
    {
        if( (n = read(childFd, outP + got, (outLen - 1 - got))) > 0 )
        {
            got += n;
        }
        else if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }
        else
        {
            break;              // nothing waiting, or the end
        }
    }

    outP[got] = '\0';
    return(got);
}

// Collect the running child's output so far into outP, and see whether it has exited.
// Once it has, the rest of the pipe is drained into outP and the pipe is closed; a longer
// remainder is left unread and dropped. Returns 1 still running, 0 exited (childStatus() has
// its status), -1 no child.
int
childPoll(char *outP, size_t outLen)
{
int status;
pid_t pid;

    if( outLen > 0 )
    {
        outP[0] = '\0';
    }

    if( !childPid )
    {
        return(-1);
    }

    if( (pid = waitpid(childPid, &status, WNOHANG)) == 0 )
    {
        if( outLen > 0 )
        {
            drainOutput(outP, outLen);
        }
        return(1);
    }

    if( pid == childPid )
    {
        lastStatus = (WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status)));
    }
    else
    {
        lastStatus = -1;
    }

    if( outLen > 0 )
    {
        drainOutput(outP, outLen);
    }

    close(childFd);
    childFd = -1;
    childPid = 0;
    return(0);
}

// The exit status of the last child to finish: its exit code, 128 plus the signal that ended
// it, or -1 if it could not be collected.
int
childStatus(void)
{
    return(lastStatus);
}

// Whether a child is running.
bool
childRunning(void)
{
    return(childPid != 0);
}

// Write err to fd, for launchDetached's parent to read. Only system calls: it runs after fork.
static void
sendErrno(int fd, int err)
{
    while( (write(fd, &err, sizeof(err)) < 0) && (errno == EINTR) )
    {
    }
}

// Start argvP[0] with arguments argvP, detached: in a session of its own and not our child, so
// that it outlives pdp1central, no SIGHUP to our group reaches it, and nothing here waits for it.
// argvP[0] is looked for on PATH unless it holds a slash. stdin, stdout and stderr are /dev/null.
// It does not use childStart's slot, so it works while a script or a dialog runs.
// Returns false, with why in errorP, if the program could not be started: "NAME: not found" when
// there is no such program. An exec still not done after 2 s is taken as started.
bool
launchDetached(char *const argvP[], char *errorP, size_t errorLen)
{
struct pollfd pfd;
int pipeFds[2];
int nullFd, err, status;
ssize_t n;
pid_t pid;

    if( errorLen > 0 )
    {
        errorP[0] = '\0';
    }

    // The write end is closed by a successful exec, so the read end sees its end; an exec that
    // fails writes its errno there first.
    if( pipe2(pipeFds, O_CLOEXEC) )
    {
        snprintf(errorP, errorLen, "cannot start %s: %s", argvP[0], strerror(errno));
        return(false);
    }

    if( (pid = fork()) < 0 )
    {
        snprintf(errorP, errorLen, "cannot start %s: %s", argvP[0], strerror(errno));
        close(pipeFds[0]);
        close(pipeFds[1]);
        return(false);
    }

    if( pid == 0 )
    {
        // The first child makes the session and leaves at once. The second, the program, is
        // then no child of ours, and, not being the session's leader, can take no terminal.
        close(pipeFds[0]);
        defaultSignals();
        setsid();
        if( (pid = fork()) < 0 )
        {
            sendErrno(pipeFds[1], errno);
            _exit(1);
        }
        if( pid > 0 )
        {
            _exit(0);
        }

        if( (nullFd = open("/dev/null", O_RDWR)) >= 0 )
        {
            dup2(nullFd, 0);
            dup2(nullFd, 1);
            dup2(nullFd, 2);
            if( nullFd > 2 )
            {
                close(nullFd);
            }
        }

#ifdef CLOSE_RANGE_CLOEXEC
        // None of our own descriptors, such as the window's connection, lives on in the program.
        close_range(3, ~0U, CLOSE_RANGE_CLOEXEC);
#endif
        execvp(argvP[0], argvP);
        sendErrno(pipeFds[1], errno);
        _exit(127);
    }

    close(pipeFds[1]);
    while( (waitpid(pid, &status, 0) < 0) && (errno == EINTR) )
    {
    }

    err = 0;
    pfd.fd = pipeFds[0];
    pfd.events = POLLIN;
    if( poll(&pfd, 1, LAUNCH_WAIT_MS) == 1 )
    {
        while( ((n = read(pipeFds[0], &err, sizeof(err))) < 0) && (errno == EINTR) )
        {
        }
        if( n != sizeof(err) )
        {
            err = 0;            // the end, with nothing written: exec worked
        }
    }
    close(pipeFds[0]);

    if( err == ENOENT )
    {
        snprintf(errorP, errorLen, "%s: not found", argvP[0]);
    }
    else if( err )
    {
        snprintf(errorP, errorLen, "%s: %s", argvP[0], strerror(err));
    }

    return(err == 0);
}

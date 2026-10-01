// Runs one child program at a time without blocking the event loop: pdp1control.sh for start,
// stop, restart and the reloads, and the Tk file dialogs.
// The child gets its own session (setsid), so the emulator the script starts is not in
// pdp1central's session and outlives it. Its stdin is /dev/null, and its stdout and stderr go to
// a non-blocking pipe that childPoll() drains.
// The child is judged finished when waitpid() reports it, not at the pipe's end: a program the
// script starts in the background (screen, nohup) can hold the pipe open indefinitely.

#include <errno.h>
#include <fcntl.h>
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

// Start argvP[0] with arguments argvP, unless a child is still running.
// Returns false if one is, or if the child could not be started.
bool
childStart(char *const argvP[])
{
int pipeFds[2];
int nullFd, sig;
sigset_t none;
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
        // An ignored signal stays ignored across exec, and the emulator the script starts must
        // not inherit whatever the app or SDL did with these.
        for( sig = 1; sig < NSIG; sig++ )
        {
            if( (sig != SIGKILL) && (sig != SIGSTOP) )
            {
                signal(sig, SIG_DFL);
            }
        }

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
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

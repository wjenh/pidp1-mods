/*
 * A test program to drive t30dpy in various ways.
 *
 * Test 1 draws adjacent filled squares in ascending intensity
 * Test 2 draws 8 lines in ascending intensity
 * Test 3 draws adjacent filled squares in ascending intensity
 * Test 4 draws long rectangles in ascending intensity
 *
 * Each test runs for TESTTIME seconds.
 * This listens on port 3411.
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>

#define TESTTIME    8       // number of seconds a test lasts

int waitForDpy(int port);
bool drawDot(int fd, int x, int y, int intensity);
uint64_t now(void);
bool hasElapsed(int msecs);

bool test1(int fd);
bool test2(int fd);
bool test3(int fd);
bool test4(int fd);

int
main(int argc, char **argv)
{
int dpyFD;
char line[246];

    printf("Test 1 - intensity strip\n");
    printf("Test 2 - intensity lines\n");
    printf("Test 3 - intensity and space increasing dots\n");
    printf("Test 3 - intensity increasing long rectangles\n");
    printf("Waiting for t30dpy to connect...\n");
    if( (dpyFD = waitForDpy(3411)) < 0 )
    {
        printf("Connection wait failed.\n");
        exit(1);
    }

    for(;;)
    {
        printf("Type a test number or q to quit,\n");
        fgets(line, sizeof(line), stdin);
        switch( line[0] )
        {
        case 'q':
            close(dpyFD);
            exit(0);

        case '1':
            test1(dpyFD);
            break;

        case '2':
            test2(dpyFD);
            break;

        case '3':
            test3(dpyFD);
            break;

        case '4':
            test4(dpyFD);
            break;

        default:
            printf("Test number or q to exit.\n");
            break;
        }
    }
}

// Draw 8 filled squares in increasing intensity
bool
test1(int fd)
{
int i, j;
int x, y, intensity;

    while( !hasElapsed(TESTTIME * 1000) )
    {
        y = 450;
        for( i = y; i <= (y + 50); ++i )
        {
            x = 400;
            for( intensity = 7; intensity >= 0; --intensity )
            {
                j = x + 50;
                while( x < j )
                {
                    if( !drawDot(fd, x++, i, intensity) )
                    {
                        return(false);
                    }
                }
            }
        }
    }

    return(true);
}

// Draw 8 lines in increasing intensity
bool
test2(int fd)
{
int i, j;
int x, y, intensity;

    while( !hasElapsed(TESTTIME * 1000) )
    {
        y = 450;
        for( intensity = 0; intensity < 8; ++intensity )
        {
            x = 400;
            while( x < 800  )
            {
                if( !drawDot(fd, x++, y, intensity) )
                {
                    return(false);
                }
            }

            y += 20;
        }
    }

    return(true);
}

// Draw 8 lines in increasing intensity of increasingly spaced dots
bool
test3(int fd)
{
int i, j;
int x, y, intensity;
int dotSpace;

    while( !hasElapsed(TESTTIME * 1000) )
    {
        y = 450;
        for( intensity = 0; intensity < 8; ++intensity )
        {
            dotSpace = 1;
            x = 400;
            while( dotSpace < 20  )
            {
                if( !drawDot(fd, x += dotSpace++, y, intensity) )
                {
                    return(false);
                }
            }

            y += 20;
        }
    }

    return(true);
}

// Draw 8 filled long rectanges in increasing intensity
bool
test4(int fd)
{
int i, j;
int x, y, intensity;

    while( !hasElapsed(TESTTIME * 1000) )
    {
        y = 450;
        for( intensity = 7; intensity >= 0; --intensity, y += 55 )
        {
            for( i = y; i <= (y + 20); ++i )
            {
                x = 100;
                j = 900;
                while( x < j )
                {
                    if( !drawDot(fd, x++, i, intensity) )
                    {
                        return(false);
                    }
                }
            }
        }
    }

    return(true);
}

int
waitForDpy(int port)
{
int fd, socketFD, opt;
struct sockaddr_in address;
socklen_t addrlen;

    if( (fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        printf("Socket open failed, errno %d\n", errno);
        return(-1);
    }
    
    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if( bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 )
    {
        printf("Socket bind failed, errno %d\n", errno);
        close(fd);
        return(-1);
    }

    if( listen(fd, 1) < 0 )
    {
        printf("Listen failed, errno %d\n", errno);
        close(fd);
        return(-1);
    }

    addrlen = sizeof(address);

    if( (socketFD = accept(fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0 )
    {
        printf("Accept failed, errno %d\n", errno);
        close(fd);
        return(-1);
    }
    
    close(fd);
    return( socketFD );
}

// Write one dot to display.
// Returns true for success, false for fail.
bool
drawDot(int fd, int x, int y, int intensity)
{
uint32_t cmd;

    cmd = (x & 0x3FF) | ((y & 0x3FF) << 10);
    cmd |= (intensity & 7) << 20;
    return( write(fd, &cmd, sizeof(cmd)) == sizeof(cmd) );
}

bool
hasElapsed(int msecs)
{
static uint64_t endTime = 0;

    if( endTime == 0 )
    {
        endTime = now() + ((uint64_t)msecs * 1000 * 1000);    // time is in ns
    }

    if( now() >= endTime )
    {
        endTime = 0;
        return(true);
    }

    return(false);
}

// Get the current time in ns.
uint64_t
now()
{
struct timespec tm;
uint64_t now;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    now = tm.tv_nsec;
    now += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;

    return(now);
}

#ifndef COMMON_H
#define COMMON_H

// 8-Apr-2026 wje initial cleanup

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;
#define nil NULL
#define NEVER (~0)

#define nelem(array) (sizeof(array)/sizeof(array[0]))

struct PortHandler
{
    int port;
    void (*handle)(int fd, void *arg);
};

// pollfd.c
#define FD_STRUCT_H
typedef struct
{
    int fd;
    int ready;
    int id;
} FD;

void panic(const char *fmt, ...);
int hasinput(int fd);
int socketlisten(int port);
int dial(const char *host, int port);
int serve1(int port);
void serveN(struct PortHandler *ports, int nports, void *arg);
void nodelay(int fd);

void *createseg(const char *name, size_t sz);
void *attachseg(const char *name, size_t sz);

void inittime(void);
u64 gettime(void);
void nsleep(u64 ns);

char **split(char *line, int *pargc);

void startpolling(void);
void waitfd(FD *fd);
void closefd(FD *fd);

#endif

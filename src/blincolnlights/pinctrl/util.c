// util.c -- Small device-tree and raw-file-read helpers used by the pinctrl/gpiolib code.
// See util.h for the architectural scope and per-function contracts. Function/API names
// declared in util.h are kept as-is here (external callers, e.g. gpiolib.c, depend on
// them); purely local names (the static helper functions and their internal locals) follow
// the project's camelHump convention.

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "util.h"

// We're actually going to cheat and cast the pointers, but define a structure to keep the
// compiler happy -- DT_SUBNODE_HANDLE (util.h) is really just a wrapped DIR *.
struct dt_subnode_iter
{
    DIR *dh;
};

// Device-tree root directory every dt_*() function below resolves node paths against; set
// once via dt_set_path().
const char *dtpath;

// Shared implementation for read_text_file()/read_file(): opens fname in the given mode,
// reads it in full into a freshly malloc()'d buffer, and closes it. Returns the buffer
// (caller must free()), or NULL if the file could not be opened or the read came up short.
// If plenP is non-NULL, *plenP receives the byte count read.
static void *
doReadFile(const char *fname, const char *mode, size_t *plenP)
{
FILE *fileP;
void *bufP;
long len;

    fileP = fopen(fname, mode);
    if( fileP == NULL )
    {
        return(NULL);
    }

    fseek(fileP, 0, SEEK_END);
    len = ftell(fileP);
    if( plenP )
    {
        *plenP = len;
    }

    bufP = malloc(len);
    fseek(fileP, 0, SEEK_SET);
    if( bufP )
    {
        if( fread(bufP, 1, len, fileP) != (size_t)len )
        {
            free(bufP);
            bufP = NULL;
        }
    }

    fclose(fileP);
    return( (char *)bufP );
}

// See util.h.
char *
read_text_file(const char *fname, size_t *plen)
{
    return( doReadFile(fname, "rt", plen) );
}

// See util.h.
void *
read_file(const char *fname, size_t *plen)
{
    return( doReadFile(fname, "rb", plen) );
}

// See util.h. No return value.
void
dt_set_path(const char *path)
{
    dtpath = path;
}

// See util.h.
char *
dt_read_prop(const char *node, const char *prop, size_t *plen)
{
char filename[FILENAME_MAX];
size_t len;

    len = snprintf(filename, sizeof(filename), "%s%s/%s", dtpath, node, prop);
    if( len >= sizeof(filename) )
    {
        assert(0);
        return(NULL);
    }

    filename[sizeof(filename) - 1] = '\0';

    return( read_file(filename, plen) );
}

// See util.h.
uint32_t *
dt_read_cells(const char *node, const char *prop, unsigned *num_cells)
{
uint8_t *bufP;
size_t len, i;

    bufP = (uint8_t *)dt_read_prop(node, prop, &len);
    if( bufP )
    {
        // Byte-swap each big-endian 32-bit cell into host order in place; any trailing
        // partial cell (fewer than 4 bytes left) is silently dropped.
        for( i = 0; (i + 3) < len; i += 4 )
        {
            *(uint32_t *)(bufP + i) = (bufP[i] << 24) + (bufP[i + 1] << 16) +
                (bufP[i + 2] << 8) + (bufP[i + 3] << 0);
        }

        *num_cells = i >> 2;
    }

    return( (uint32_t *)bufP );
}

// See util.h.
uint64_t
dt_extract_num(const uint32_t *cells, int size)
{
uint64_t val;
int i;

    val = 0;

    // PCIe uses 3 cells for an address, but we can ignore the first cell. In this case,
    // the big-endian representation makes it easy because the unwanted portion is
    // shifted off the top.
    for( i = 0; i < size; i++ )
    {
        val = (val << 32) | cells[i];
    }

    return(val);
}

// See util.h.
uint64_t
dt_read_num(const char *node, const char *prop, size_t size)
{
unsigned numCells;
uint32_t *cellsP;
uint64_t val;

    cellsP = dt_read_cells(node, prop, &numCells);
    val = 0;

    if( cellsP )
    {
        if( size <= numCells )
        {
            val = dt_extract_num(cellsP, size);
        }

        dt_free(cellsP);
    }

    return(val);
}

// See util.h.
uint32_t
dt_read_u32(const char *node, const char *prop)
{
    return( dt_read_num(node, prop, 1) );
}

// Applies one level of device-tree address translation: given a "ranges" cell array
// (entries of the form <child addr> <parent addr> <size>, each field npa/nps/nca cells
// wide respectively, big-endian), finds the entry whose child range contains addr and
// returns addr translated into the parent's address space. If no matching entry is found,
// addr is returned unchanged (matches the Linux kernel's own device-tree address
// resolution fallback behavior).
static uint64_t
dtTranslateAddr(const uint32_t *ranges, unsigned rangesCells, int npa, int nps, int nca,
    uint64_t addr)
{
unsigned pos;
uint64_t ca, pa, ps;

    pos = 0;

    while( (pos + npa + nps + nca) <= rangesCells )
    {
        ca = dt_extract_num(ranges + pos, nca);
        pa = dt_extract_num(ranges + pos + nca, npa);
        ps = dt_extract_num(ranges + pos + nca + npa, nps);

        if( (addr >= ca) && (addr <= (ca + ps)) )
        {
            addr -= ca;
            addr += pa;
            break;
        }

        pos += npa + nps + nca;
    }

    return(addr);
}

// See util.h.
uint64_t
dt_parse_addr(const char *node)
{
char buf1[FILENAME_MAX], buf2[FILENAME_MAX];
char *parentP, *nextParentP;
uint32_t *rangesP;
unsigned rangesCells;
uint64_t addr;
unsigned npa, nps, nca;
char *tmpP, *slashP;

    rangesP = NULL;
    rangesCells = 0;
    addr = INVALID_ADDRESS;
    nca = 0;

    parentP = buf1;
    nextParentP = buf2;

    while( 1 )
    {
        strcpy(parentP, node);
        slashP = strrchr(parentP, '/');
        if( !slashP )
        {
            return(INVALID_ADDRESS);
        }

        if( slashP == parentP )
        {
            slashP[1] = '\0';
        }
        else
        {
            slashP[0] = '\0';
        }

        npa = dt_read_u32(parentP, "#address-cells");
        nps = dt_read_u32(parentP, "#size-cells");
        if( !npa || !nps )
        {
            addr = INVALID_ADDRESS;
            break;
        }

        if( addr == INVALID_ADDRESS )
        {
            addr = dt_read_num(node, "reg", npa);
        }
        else if( rangesP )
        {
            addr = dtTranslateAddr(rangesP, rangesCells, npa, nps, nca, addr);
            dt_free(rangesP);
            rangesP = NULL;
        }

        if( parentP[1] == '\0' )
        {
            break;
        }

        rangesP = dt_read_cells(parentP, "ranges", &rangesCells);
        nca = npa;
        node = parentP;

        // Swap parentP and nextParentP so the next iteration builds the new parent path
        // into whichever buffer isn't 'node' right now.
        tmpP = parentP;
        parentP = nextParentP;
        nextParentP = tmpP;
    }

    dt_free(rangesP);

    return(addr);
}

// See util.h. No return value.
void
dt_free(void *value)
{
    free(value);
}

// See util.h.
DT_SUBNODE_HANDLE
dt_open_subnodes(const char *node)
{
char dirpath[FILENAME_MAX];
size_t len;

    len = snprintf(dirpath, sizeof(dirpath), "%s%s", dtpath, node);
    if( len >= sizeof(dirpath) )
    {
        assert(0);
        return(NULL);
    }

    return( (DT_SUBNODE_HANDLE)opendir(dirpath) );
}

// See util.h.
const char *
dt_next_subnode(DT_SUBNODE_HANDLE handle)
{
DIR *dirP;
struct dirent *dentP;

    dirP = (DIR *)handle;
    dentP = readdir(dirP);

    return( dentP ? dentP->d_name : NULL );
}

// See util.h. No return value.
void
dt_close_subnodes(DT_SUBNODE_HANDLE handle)
{
DIR *dirP;

    dirP = (DIR *)handle;
    closedir(dirP);
}

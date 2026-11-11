// util.h -- Small device-tree and raw-file-read helpers used by the pinctrl/gpiolib code.
// Architectural scope: reads /proc/device-tree entries (address-cells, ranges, reg, etc.)
// so gpiolib.c can locate and size the memory-mapped GPIO register windows without any
// hardcoded per-SoC addresses. No state beyond dtpath (set once via dt_set_path()).
// Dependencies: none beyond the C standard library; every function here is a thin wrapper
// over open()/read()-style file access plus device-tree "big-endian cell" parsing.
#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>
#include <stddef.h>

#define INVALID_ADDRESS ((uint64_t)~0)
#define ROUND_UP(n, d) ((((n) + (d) - 1) / (d)) * (d))
#define UNUSED(x) (void)(x)

// Opaque handle for an in-progress device-tree subnode directory scan (dt_open_subnodes()/
// dt_next_subnode()/dt_close_subnodes()). The real definition (a wrapped DIR *) lives in
// util.c; callers only ever hold and pass back the pointer.
typedef struct dt_subnode_iter *DT_SUBNODE_HANDLE;

// Reads the whole named file as text (mode "rt"). Returns a malloc()'d NUL-unterminated
// buffer, or NULL if the file could not be opened or read in full. If plen is non-NULL,
// *plen receives the byte count read. Caller must free() the returned buffer.
char *read_text_file(const char *fname, size_t *plen);

// Reads the whole named file as raw binary (mode "rb"). Same buffer/ownership/plen
// contract as read_text_file() above. Returns NULL on any open/read failure.
void *read_file(const char *fname, size_t *plen);

// Records the device-tree root directory (e.g. "/proc/device-tree") that every dt_*()
// function below resolves node paths against. No return value.
void dt_set_path(const char *path);

// Reads device-tree property 'prop' under 'node' (relative to the path set by
// dt_set_path()) as a raw byte buffer. Returns a malloc()'d buffer (free with dt_free()),
// or NULL if the property does not exist or the resolved path is too long. If len is
// non-NULL, *len receives the byte count read.
char *dt_read_prop(const char *node, const char *prop, size_t *len);

// Reads a device-tree property as an array of big-endian 32-bit cells, byte-swapping each
// one into host order in place. Returns a malloc()'d array (free with dt_free()), or NULL
// if the property is absent; *num_cells receives the number of whole 4-byte cells found
// (any trailing partial cell is silently dropped).
uint32_t *dt_read_cells(const char *node, const char *prop, unsigned *num_cells);

// Packs the first 'size' entries of a big-endian device-tree cell array into a single
// 64-bit value (each cell contributes 32 bits, most-significant cell first). Returns the
// packed value.
uint64_t dt_extract_num(const uint32_t *cells, int size);

// Reads device-tree property 'prop' under 'node' and packs its first 'size' cells into a
// 64-bit value via dt_extract_num(). Returns 0 if the property is absent or shorter than
// 'size' cells, else the packed value.
uint64_t dt_read_num(const char *node, const char *prop, size_t size);

// Convenience wrapper for the common case of a single-cell (32-bit) property. Returns the
// property's value, or 0 if it does not exist.
uint32_t dt_read_u32(const char *node, const char *prop);

// Resolves a device-tree node's "reg" address by walking up the parent chain and applying
// each ancestor's "ranges" translation, exactly like the Linux kernel's own device-tree
// address resolution. Returns the translated address, or INVALID_ADDRESS if any required
// property (#address-cells, #size-cells, reg) is missing along the way.
uint64_t dt_parse_addr(const char *node);

// Frees a buffer previously returned by dt_read_prop()/dt_read_cells()/dt_parse_addr()'s
// internal allocations. No return value. Safe to call with NULL (matches free()).
void dt_free(void *value);

// Opens 'node' (relative to the path set by dt_set_path()) for a subnode-name scan.
// Returns a handle to pass to dt_next_subnode()/dt_close_subnodes(), or NULL if the
// resolved path is too long or the directory could not be opened.
DT_SUBNODE_HANDLE dt_open_subnodes(const char *node);

// Returns the next subnode (directory entry) name for an open dt_open_subnodes() scan, or
// NULL once the scan is exhausted. The returned pointer is only valid until the next call
// on the same handle (owned by the underlying directory-stream implementation).
const char *dt_next_subnode(DT_SUBNODE_HANDLE handle);

// Closes a scan handle opened by dt_open_subnodes(). No return value.
void dt_close_subnodes(DT_SUBNODE_HANDLE handle);

#endif

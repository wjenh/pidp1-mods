#ifndef TYPE340CHARS_H
#define TYPE340CHARS_H
/*
 * This defines the special control characters for the Type 340 character set.
 * The TYPE340END character MUST BE the last character in a string.
*/

#define TYPE340LF       033
#define TYPE340CR       034
#define TYPE340UPPER    035
#define TYPE340LOWER    036
#define TYPE340END      037
#define TYPE340BLOB     000
#define TYPE340NL       100 // special case, doesn't really exist, just a marker
#define TYPE340AUTO     101 // special case, doesn't really exist, just a marker
#define TYPE340NOEND    102 // special case, doesn't really exist, just a marker

#endif

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

// More markers, returned by the escape routine.
// They sit above the 6-bit character range so they can never be mistaken for a code.
#define TYPE340CONT     103 // backslash-newline, a line continuation, store nothing
#define TYPE340PLAIN    104 // the escaped character is an ordinary ASCII character
#define TYPE340BKSP     105 // backspace, lower-set code 072
#define TYPE340SUBSCR   106 // subscript, lower-set code 073
#define TYPE340SUPER    107 // superscript, lower-set code 077

// The lower-set codes those three markers stand for.
#define TYPE340CODE_BKSP    072
#define TYPE340CODE_SUBSCR  073
#define TYPE340CODE_SUPER   077

#endif

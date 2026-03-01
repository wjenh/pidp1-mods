// This contains all the help message support for ad1.
// Also list them in helpmsgs.h so other code can find them.

#include <stdio.h>
#define NIL (char *)0

char *helpMsg[] = {
    "All commands can be shortened to the characters in capitals.",
    "Numeric values that are input default to base 8, but that can be changed via base.",
    "If an input number isn't valid for current base, an error will be given,.",
    "unless an explicit override has been used",
    "0b1001 - force binary",
    "0123 - force octal",
    "0d123 - force decimal",
    "0x123 - force hexadecimal",
    "Addresses are full 16 bit values, any bank can be addressed.",
    "See the documentation of bank addressing.",
    "Entering just a return is the same as list with no arguments, print the next set of lines.",
    "Valid commands are:",
    NIL};

char *dotHelp[] = {
    "The dot command prints the value of the last used memory address using the last output format used.",
    "When a dot is used in an expression, its value is the last used memory address.",
    "Examples:",
    "As a command, .",
    "As an expression, .+3",
    NIL};

char *exitHelp[] = {
    "The quit command exits ad1.",
    "Any breakpoints are deleted before exiting.",
    "Example: q",
    NIL};

char *breakHelp[] = {
    "The break command sets a breakpoint at the given address, numeric or symbolic.",
    "An expression can also be used to compute an address.",
    "A count can optionally be given, which is how many times it must be hit to be reported.",
    "When that location is executed and the count reached, the pidp-1 will halt and the breakpoint reported.",
    "The program can then be continued after any other desired commnds are used.",
    "A breakpoint should not be set on a location that is in data, it will never be hit.",
    "Examples:",
    "b loop",
    "b loop 10",
    "b 104",
    "b foo+10",
    NIL};

char *listHelp[] = {
    "The list command shows the content of the original source file or of a register",
    "If it is not automatically founnd, the 'file' command can be used to locate it.",
    "For full functionality, the actual file should be produced by am1.",
    "If just the original source file is available, positioning by line number will the the only available.",
    "List with no arguments lists the next set of lines, or if none have been listed before, lines",
    "starting from the first line.",
    "A numeric value lists from that line in the file.",
    "A numeric value prefixed by an @ lists from the line at that address.",
    "A string value lists from the location of that symbol in the file if a symbol table is available.",
    "If there is no such symbol, an error is given.",
    "An optional format specifier can be added. This does not change the inpupt base, only the display",
    "Specifiers are:",
    "b - binary",
    "o - octal",
    "d - decimal",
    "x - hexadecimal",
    "c - 1's complement, sign extend the 18 bit value and show the 1's complement result",
    "a - ascii, 2 chars per word",
    "f - flex, 3 chars per word, shift state remembered until another format is used",
    "Also see the window command and register help.",
    "Examples:",
    "sh loop",
    "sho loop x",
    "show ac b",
    NIL};

char *windowHelp[] = {
    "The window command sets the number of lines before and after the line being displayed by",
    "the line command. The default is 3",
    "If the window would cause lines before the first or after the last to be displayed,",
    "only valid lines will be displayed,",
    "Example: win 3",
    NIL};

char *bankHelp[] = {
    "The bank command sets the default memory bank to the value given,",
    "which must be 0-15.",
    "See the documentation for important details.",
    "Example: bank 10",
    NIL};

char *baseHelp[] = {
    "The base command sets the default numeric base to the value given,",
    "which must be 2, 8, 10, or 16",
    "Example: base 10",
    NIL};

char *continueHelp[] = {
    "The continue command continues execution if the pidp-1 is halted.",
    "It has no effect if it is not halted.",
    "Example: continue",
    NIL};

char *deleteHelp[] = {
    "The delete command removes an existing breakpoint by its number.",
    "or if no number is given, all breakpoints.",
    "An error will be given if there is no breakpoint.",
    "Examples:",
    "del 10",
    "del",
    "If the word watch follows the delete, then this applies to watches.",
    "Again, if no number is given, all watches are deleted.",
    "Example: del w 10",
    NIL};

char *disableHelp[] = {
    "The disable command disables but does not remove  an existing breakpoint by its number.",
    "An error will be given if there is no breakpoint.",
    "Example: disa 10",
    "If the word watch follows the disable, then this applies to watches.",
    "Example: di w 10",
    NIL};

char *enableHelp[] = {
    "The enable command enables a disabled breakpoint by its number.",
    "An error will be given if there is no breakpoint.",
    "Example: en 10",
    "If the word watch follows the disable, then this applies to watches.",
    "Example: en w 10",
    NIL};

char *watchHelp[] = {
    "The watch command monitors e given address, numeric or symbolic.",
    "An expression can also be used to compute an address.",
    "If no value is given after the address, then any change to the contents of that address",
    "cause the pidp-1 to halt and the watch reported.",
    "The program can then be continued after any other desired commnds are used.",
    "A watch can be set on any memory location, but if it never changes, nothing will happen.",
    "Examples:",
    "w loop",
    "wat loop 10",
    "wa foo+10",
    NIL};

char *nextHelp[] = {
    "The next command increments the current address by one ane repeats the last show command at that address.",
    "It does not affect the pidp-1, it just chnages ad1's location counter.",
    "The address is kept as a full 16 bit address, so a next at the end of a bank will advance to the next bank.",
    "Example: n",
    NIL};

char *setHelp[] = {
    "The set command changes the values of of either memory locations or of some registers.",
    "The address can be numeric or symbolic.",
    "An expression can also be used to compute an address.",
    "The value is in the current base unless a numeric override, e.g. 0d12, is used.",
    "If instead of an address, one of the register names is given, the contents of that register will be set.",
    "Only the ac, io, pf,pf1-pf6, and pc can be set.",
    "Use help reg for help on registers.",
    "Examples:",
    "se 100 123",
    "set cnt 0xff",
    "se io 0177",
    NIL};

char *showHelp[] = {
    "The show command shows the values of of either memory locations or of registers.",
    "The address can be numeric or symbolic.",
    "An expression can also be used to compute an address.",
    "The value will be printed in the current base unless a second parameter is given,",
    "which must be one of b, o, d, x, or c, meaning binary, octal, decimal, hex, or 1's complement.",
    "If instead of an address, one of the register names is given, the contents of that register will be shown.",
    "Use help reg for help on registers.",
    "Examples:",
    "sh 100",
    "sh cnt",
    "sh io x",
    NIL};

char *startHelp[] = {
    "The start command begins execution at the given address.",
    "It is equivalent to using the start switch.",
    "A full 16 bit address is accepted.",
    "Example: start 100",
    NIL};

char *stopHelp[] = {
    "The stop command halts the pidp-1.",
    "It is equivalent to using the stop switch.",
    "Example: stop",
    NIL};

char *stepHelp[] = {
    "The step command executes one instruction and halts.",
    "It is equivalent to using the continue switch in single-instruction mode.",
    "Example: step",
    NIL};

// Extra help
char *numberHelp[] = {
    "Numbers can be expressed in multiple bases, 2, 8, 10, and 16.",
    "Normally, a number is expected to be in the base set by the base command, initially base 8.",
    "However, an override can be used by having the first 2 characters be 0b, 0o, 0d, or 0x.",
    "These mean the obvious, binary, octal, decimal, or hexadecimal.",
    "Using an override does not change the current default base.",
    "Example: 0x3f or 0x3F",
    NIL};

char *expressionHelp[] = {
    "In most places an address or value can be used, an expression can be used.",
    "An expression is composed of numbers and symbols connected by operators.",
    "The operators are +, -, *, /, &, |, ^, ~, (, ).",
    "See the documentation for precedence, it follows C precedence.",
    "Computatios are done in 2's complement math, but this is only within a math operation.",
    "For examle, foo - 1 converts foo to 2's complement does the subtraction, then converts back.",
    "Logical operators do not cause conversions.",
    "Thus, values will appear as they would in the pidp-1.",
    "The special symbol ', dot, means 'the last address used', similar to its use in an assembler.",
    "This is transparent to the user.",
    "Examples:",
    ".+10",
    "counter+10",
    "counter*(10+.)",
    NIL};

char *registerHelp[] = {
    "The valid registers are ac, io, pf,pf1-pf6, tw, pc, as, ma, mb, ss, break, and watch.",
    "Break and watch are pseudo-registers that will show the set breakpoints or watches.",
    "They follow the same name-shortening as the commands.",
    "The names correspond to:",
    "ac, the AC register",
    "io, the IO register",
    "ma, the Memory Address register",
    "mb, the Memory Buffer register",
    "ss, the sense switches",
    "pf, the program flags",
    "pf1-6, individual program flags",
    "tw, the test word switches",
    "as, the address switches.",
    NIL};

// Print out a list of text, last line a NIL
void
showText(char *lines[])
{
    while( lines && *lines )
    {
        fputs(*lines++,stdout);
        fputc('\n', stdout);
    }
}

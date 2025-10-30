# MACRO1 PDP-1 Assembler Programming Guide

## Manual Page

### NAME
macro1 - a PDP-1 assembler.

### SYNOPSIS
```
macro1 [ -d -p -m -r -s -x ] inputfile inputfile...
```

### DESCRIPTION
This is a cross-assembler for PDP-1 assembly language programs. It will produce an output file in rim format only. A listing file is always produced and with an optional symbol table and/or a symbol cross-reference (concordance). The permanent symbol table can be output in a form that may be read back in so a customized permanent symbol table can be produced. Any detected errors are output to a separate file giving the filename in which they were detected along with the line number, column number and error message as well as marking the error in the listing file.

The following file name extensions are used:
- `.mac` - source code (input)
- `.lst` - assembly listing (output)  
- `.rim` - assembly output in DEC's rim format (output)
- `.prm` - permanent symbol table in form suitable for reading after the EXPUNGE pseudo-op
- `.sym` - "symbol punch" tape (for DDT, or reloading into macro)

### OPTIONS
- `-d` - Dump the symbol table at end of assembly
- `-p` - Generate a file with the permanent symbols in it (To get the current symbol table, assemble a file that has only START in it)
- `-x` - Generate a cross-reference (concordance) of user symbols
- `-r` - Output a tape using only RIM format (else output block loader)
- `-s` - Output a symbol dump tape (loader + loader blocks)
- `-S file` - Read a symbol tape back in

### DIAGNOSTICS
Assembler error diagnostics are output to an error file and inserted in the listing file. Each line in the error file has the form:

```
<filename>:<line>:<col> : error: <message> at Loc = <loc>
```

An example error message is:
```
bintst.7:17:9 : error: undefined symbol "UNDEF" at Loc = 07616
```

The error diagnostics put in the listing start with a two character error code (if appropriate) and a short message. A carat '^' is placed under the item in error if appropriate. An example error message is:

```
   17 07616 3000      DAC      UNDEF
UD undefined                   ^
   18 07617 1777      TAD   I  DUMMY
```

Undefined symbols are marked in the symbol table listing by prepending a '?' to the symbol. Redefined symbols are marked in the symbol table listing by prepending a '#' to the symbol. Examples are:

```
#REDEF   04567
 SWITCH  07612  
?UNDEF   00000
```

### AUTHORS
- Gary A. Messenbrink <gary@netcom.com> (original MACRO8)
- Bob Supnik <bob.supnik@ljo.dec.com> (MACRO7 and MACRO1 modifications)
- Phil Budne <phil@ultimate.com> (major reworking to assemble MACRO, DDT)

### COPYRIGHT
This is free software. There is no fee for using it. You may make any changes that you wish and also give it away. If you can make a commercial product out of it, fine, but do not put any limits on the purchaser's right to do the same.

## Overview

MACRO1 is a cross-assembler for PDP-1 assembly language programs. It produces output in DEC's RIM format and generates comprehensive listings with optional symbol tables and cross-references.

## Command Line Usage

```
macro1 [ -d -p -m -r -s -x ] inputfile inputfile...
```

### Options
- `-d` - Dump the symbol table at end of assembly
- `-m` - Output macro expansions
- `-p` - Generate a file with the permanent symbols in it
- `-r` - Output RIM format file (pure rim-mode tapes)
- `-s` - Output symbol punch tape to file
- `-S file` - Read a symbol punch tape from file
- `-x` - Output cross reference (concordance) to file

### File Extensions
- `.mac` - source code (input)
- `.lst` - assembly listing (output)
- `.rim` - assembly output in DEC's RIM format (output)
- `.prm` - permanent symbol table for reloading after EXPUNGE
- `.sym` - "symbol punch" tape (for DDT, or reloading into macro)

## Source File Format

### Basic Structure
- Lines can contain labels, instructions, operands, and comments
- Tabs and spaces are used for field separation
- Assembly uses 18-bit addressing with 4096 word pages

### Example Source Code
```assembly
start go

go,     lac x       ; Load accumulator with x
lup,    cma         ; Complement accumulator
        sar 4s      ; Shift accumulator right 4 positions
        add y       ; Add y to accumulator
        dac y       ; Deposit accumulator in y
        lio y       ; Load I/O register with y
        sar 4s      ; Shift accumulator right 4 positions
        add x       ; Add x to accumulator
        dac x       ; Deposit accumulator in x
        dpy         ; Display on scope
        jmp lup     ; Jump to loop

x,      200000      ; Data constant
y,      0           ; Data constant
```

## Pseudo-Instructions (Assembler Directives)

### Assembly Control
- **`START address`** - Set starting address for execution
- **`EXPUNGE`** - Clear the symbol table (for multi-segment programs)
- **`NOINPUT`** - Stop reading input

### Data Definition
- **`CONSTANTS`** - Begin constant definition section
- **`VARIABLES`** - Begin variable definition section  
- **`TEXT`** - Include literal text data

### Macro Processing
- **`DEFINE name`** - Begin macro definition
- **`REPEAT count`** - Repeat following instructions

### Numeric Base Control
- **`DECIMAL`** - Set number base to decimal (10)
- **`OCTAL`** - Set number base to octal (8) - default

### Data Conversion Functions (usable in expressions)
- **`CHARAC`** - Convert character to FIO-DEC code
- **`FLEXO`** - Convert text to Flexowriter code
- **`DECIMA`** - Interpret following number as decimal
- **`OCTAL`** - Interpret following number as octal

## PDP-1 Instruction Set

### Memory Reference Instructions
| Mnemonic | Octal  | Description |
|----------|--------|-------------|
| `AND`    | 020000 | Logical AND with memory |
| `IOR`    | 040000 | Inclusive OR with memory |
| `XOR`    | 060000 | Exclusive OR with memory |
| `XCT`    | 100000 | Execute instruction at memory location |
| `LAC`    | 200000 | Load accumulator from memory |
| `LIO`    | 220000 | Load I/O register from memory |
| `DAC`    | 240000 | Deposit accumulator in memory |
| `DAP`    | 260000 | Deposit address part in memory |
| `DIP`    | 300000 | Deposit instruction part in memory |
| `DIO`    | 320000 | Deposit I/O register in memory |
| `DZM`    | 340000 | Deposit zero in memory |
| `ADD`    | 400000 | Add memory to accumulator |
| `SUB`    | 420000 | Subtract memory from accumulator |
| `IDX`    | 440000 | Index (add one to memory, skip if zero) |
| `ISP`    | 460000 | Index and skip if positive |
| `SAD`    | 500000 | Skip if accumulator differs from memory |
| `SAS`    | 520000 | Skip if accumulator same as memory |
| `MUL`    | 540000 | Multiply |
| `DIV`    | 560000 | Divide |
| `JMP`    | 600000 | Jump |
| `JSP`    | 620000 | Jump and save program counter |
| `CAL`    | 160000 | Call subroutine |
| `JDA`    | 170000 | Jump and deposit accumulator |
| `LAW`    | 700000 | Load accumulator with word |
| `IOT`    | 720000 | Input/output transfer |
| `OPR`    | 760000 | Operate |

### Shift Instructions
| Mnemonic | Octal  | Description |
|----------|--------|-------------|
| `RAL`    | 661000 | Rotate accumulator left |
| `RIL`    | 662000 | Rotate I/O register left |
| `RCL`    | 663000 | Rotate accumulator and I/O left |
| `SAL`    | 665000 | Shift accumulator left |
| `SIL`    | 666000 | Shift I/O register left |
| `SCL`    | 667000 | Shift accumulator and I/O left |
| `RAR`    | 671000 | Rotate accumulator right |
| `RIR`    | 672000 | Rotate I/O register right |
| `RCR`    | 673000 | Rotate accumulator and I/O right |
| `SAR`    | 675000 | Shift accumulator right |
| `SIR`    | 676000 | Shift I/O register right |
| `SCR`    | 677000 | Shift accumulator and I/O right |

### Shift Counts
- `1S` through `9S` - Shift counts 1 through 9 positions

### Skip Instructions
| Mnemonic | Octal  | Description |
|----------|--------|-------------|
| `SZA`    | 640100 | Skip if accumulator zero |
| `SPA`    | 640200 | Skip if accumulator positive |
| `SMA`    | 640400 | Skip if accumulator minus (negative) |
| `SZO`    | 641000 | Skip if I/O register zero |
| `SPI`    | 642000 | Skip if program interrupt |
| `SZS`    | 640000 | Skip if sense switch set |
| `SZF`    | 640000 | Skip if flag set |

### Operate Instructions
| Mnemonic | Octal  | Description |
|----------|--------|-------------|
| `CLF`    | 760000 | Clear all flags |
| `STF`    | 760010 | Set flag |
| `CLA`    | 760200 | Clear accumulator |
| `HLT`    | 760400 | Halt |
| `CMA`    | 761000 | Complement accumulator |
| `CLC`    | 761200 | Clear link |
| `LAT`    | 762200 | Load accumulator with test word |
| `CLI`    | 764000 | Clear I/O register |
| `NOP`    | 760000 | No operation |

### Input/Output Instructions
| Mnemonic | Octal  | Description |
|----------|--------|-------------|
| `RPA`    | 730001 | Read printer A |
| `RPB`    | 730002 | Read printer B |
| `RRB`    | 720030 | Read reader binary |
| `PPA`    | 730005 | Punch printer A |
| `PPB`    | 730006 | Punch printer B |
| `TYO`    | 730003 | Type out |
| `TYI`    | 720004 | Type in |
| `DPY`    | 730007 | Display (for Spacewar!) |
| `LSM`    | 720054 | Load sequence mode |
| `ESM`    | 720055 | Enter sequence mode |
| `CBS`    | 720056 | Clear buffer start |
| `LEM`    | 720074 | Load end mode |
| `EEM`    | 724074 | Enter end mode |
| `CKS`    | 720033 | Checksum |

## Addressing Modes

### Direct Addressing
```assembly
LAC 1000    ; Load from address 1000
```

### Indirect Addressing (use `I` modifier)
```assembly  
LAC I 1000  ; Load from address contained in location 1000
```

### Current Page Addressing
- Addresses 0-177 (octal) on current page can be referenced directly
- Page 0 (addresses 0-177) is always accessible

## Symbol Definitions

### Label Definition
```assembly
LABEL,  LAC 100     ; Define LABEL at current location
```

### Constant Definition  
```assembly
CONST = 12345       ; Define CONST with value 12345
```

### Data Storage
```assembly
WORD,   777777      ; Reserve word with initial value
ZERO,   0           ; Reserve word initialized to zero
```

## Numeric Formats

### Octal (default)
```assembly
LAC 777777          ; Octal number
```

### Decimal (when in decimal mode or with DECIMA)
```assembly
DECIMAL             ; Switch to decimal mode
LAC 65535           ; Decimal number

OCTAL               ; Switch back to octal mode  
LAC DECIMA 1000     ; Force decimal interpretation
```

## Error Messages

The assembler provides detailed error diagnostics:

- **DT** - Duplicate Tag (symbol)
- **IC** - Illegal Character  
- **IE** - Illegal Expression
- **II** - Illegal Indirect (off-page reference with indirect bit set)
- **IR** - Illegal Reference (address not on current page or page zero)
- **PE** - Page Exceeded (literal table overflow)
- **RD** - Redefinition of symbol
- **ST** - Symbol Table full
- **UA** - Undefined Address (undefined symbol)

## Programming Tips

1. **Page Management**: PDP-1 uses 4096-word pages. Keep frequently accessed data and subroutines on the same page when possible.

2. **Literals**: The assembler automatically manages literal pools for constants that don't fit in instruction immediate fields.

3. **Symbol Length**: Symbol names are significant to 6 characters and use PDP-1 character encoding.

4. **Macro Usage**: Use `DEFINE` and `TERMINATE` to create reusable code sequences.

5. **Cross-Reference**: Use `-x` option to generate cross-reference listings for debugging.

## Example Complete Program

```assembly
        start main

main,   cla                 ; Clear accumulator  
        lac count           ; Load loop counter
loop,   sad limit           ; Skip if different from limit
        jmp done            ; Exit if equal
        add one             ; Increment counter
        dac count           ; Store back
        jmp loop            ; Continue loop
        
done,   hlt                 ; Halt processor

count,  0                   ; Loop counter
limit,  10                  ; Loop limit  
one,    1                   ; Constant
```

This guide covers the essential features needed to write PDP-1 assembly programs using the MACRO1 assembler. For advanced features like macro definitions and complex addressing modes, refer to the original PDP-1 documentation.
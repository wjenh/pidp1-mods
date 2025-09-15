# MACRO1 PDP-1 Assembler - AI Programming Guide

## Quick Reference for AI Systems

### Key Facts
- **Target**: PDP-1 18-bit computer (1959)
- **Address Space**: 4096 words (12-bit addressing)
- **Word Size**: 18 bits
- **Default Base**: Octal (base 8)
- **Case Sensitivity**: Case-insensitive
- **Symbol Length**: 6 characters maximum

### Command Pattern
```bash
macro1 [-d] [-p] [-m] [-r] [-s] [-x] [-S file] source.mac
```
Most common usage: `macro1 -d program.mac` (assemble with symbol dump)

## File I/O Patterns

### Input Files
- **`.mac`** - Source assembly code (required)
- **`.sym`** - Symbol tape (optional, with `-S` flag)

### Output Files (auto-generated)
- **`.lst`** - Assembly listing (always created)
- **`.rim`** - Executable binary in RIM format (always created)
- **`.prm`** - Permanent symbols (with `-p` flag)
- **`.sym`** - Symbol dump (with `-s` flag)

## Source Code Structure Patterns

### Basic Line Format
```
[LABEL,] [INSTRUCTION] [OPERAND] [;COMMENT]
```

### Critical Syntax Rules
1. **Labels end with comma**: `LOOP, LAC X`
2. **Symbols are 6 chars max**: `SYMBOL` not `VERYLONGSYMBOL`
3. **Octal by default**: `777` = 511 decimal
4. **Indirect addressing uses I**: `LAC I PTR`
5. **No spaces in numbers**: `12345` not `12 345`

## Instruction Categories & Patterns

### Memory Reference (most common)
```assembly
LAC 100     ; Load Accumulator from address 100
DAC 200     ; Deposit Accumulator to address 200
ADD 300     ; Add memory to accumulator
JMP LOOP    ; Jump to LOOP
```
**Pattern**: `OPCODE ADDRESS` or `OPCODE I ADDRESS` (indirect)

### Immediate/Operate Instructions
```assembly
LAW 777     ; Load Accumulator with literal 777
CLA         ; Clear Accumulator
CMA         ; Complement Accumulator  
HLT         ; Halt
```
**Pattern**: `OPCODE [VALUE]`

### Shift Instructions
```assembly
SAR 4S      ; Shift Accumulator Right 4 positions
RAL 1S      ; Rotate Accumulator Left 1 position
```
**Pattern**: `SHIFT_OP COUNT` where COUNT is `1S` through `9S`

## Essential Pseudo-Instructions

### Program Structure
```assembly
START address   ; Set starting execution address (required)
EXPUNGE         ; Clear symbol table
```

### Data Definition
```assembly
LABEL,    12345    ; Define word with value
BUFFER,   0        ; Reserve word, initialize to 0
TABLE = 1000       ; Define symbolic constant
```

### Base Control
```assembly
DECIMAL       ; Switch to decimal mode
OCTAL         ; Switch to octal mode (default)
```

## Common Code Patterns

### Loop Structure
```assembly
START MAIN

MAIN,   LAC COUNT     ; Load counter
LOOP,   SAD LIMIT     ; Skip if different from limit  
        JMP DONE      ; Exit when equal
        ; ... loop body ...
        ADD ONE       ; Increment counter
        DAC COUNT     ; Store counter
        JMP LOOP      ; Continue loop

DONE,   HLT           ; Stop

COUNT,  0             ; Loop variable
LIMIT,  10            ; Loop limit
ONE,    1             ; Increment value
```

### Subroutine Pattern
```assembly
        JSP SUBR      ; Call subroutine (saves return in SUBR)
        ; ... continues here ...

SUBR,   0             ; Return address stored here
        ; ... subroutine body ...
        JMP I SUBR    ; Return via indirect jump
```

## Error Patterns to Avoid

### Common AI Mistakes
1. **Wrong address format**: Use `100` not `0x100` or `100h`
2. **Missing comma on labels**: `LOOP LAC X` → `LOOP, LAC X`
3. **Decimal in octal mode**: `10` = 8 decimal, use `DECIMAL` or `12` for 10 decimal
4. **Long symbol names**: `VERYLONGNAME` → `VLONG` (6 char max)
5. **Wrong indirection**: `LAC (PTR)` → `LAC I PTR`

### Symbol Definition Errors
```assembly
; WRONG
LABEL LAC 100        ; Missing comma
VERYLONGNAME, 123    ; Name too long

; CORRECT  
LABEL, LAC 100       ; Comma after label
VLONG, 123           ; 6 chars or less
```

## Debugging Information

### Error Code Meanings
- **DT** - Duplicate Tag (symbol defined twice)
- **IC** - Illegal Character
- **UA** - Undefined Address (symbol not defined)
- **IR** - Illegal Reference (off-page address)
- **PE** - Page Exceeded (out of memory)

### Useful Assembly Flags
- **`-d`** - Dump symbol table (essential for debugging)
- **`-x`** - Cross-reference (shows where symbols used)
- **`-m`** - Show macro expansions

## AI Code Generation Guidelines

### When Writing PDP-1 Assembly:

1. **Start with structure**:
   ```assembly
   START MAIN
   
   MAIN, [your code here]
         HLT
   
   ; Data section
   VAR1, 0
   VAR2, 0
   ```

2. **Use meaningful 6-char labels**:
   - `LOOP1`, `LOOP2` for loops
   - `TEMP1`, `TEMP2` for temporaries  
   - `SUBR1`, `SUBR2` for subroutines

3. **Remember octal default**:
   - `10` = 8 decimal
   - `377` = 255 decimal
   - `777777` = -1 (18-bit two's complement)

4. **Check addressing**:
   - Direct: `LAC VAR`
   - Indirect: `LAC I PTR`
   - Immediate: `LAW 123`

5. **Include comments for clarity**:
   ```assembly
   LAC COUNT     ; Load loop counter
   SAD LIMIT     ; Check against limit
   JMP DONE      ; Exit if equal
   ```

## Complete Minimal Example
```assembly
        START MAIN

MAIN,   LAC NUM1      ; Load first number
        ADD NUM2      ; Add second number  
        DAC RESULT    ; Store result
        HLT           ; Stop program

NUM1,   123           ; First operand
NUM2,   456           ; Second operand  
RESULT, 0             ; Sum storage
```

This pattern works for most simple PDP-1 programs and can be extended as needed.
/* optdecode.c - the am1 optimizer's instruction decoder and spelling classifier
 *
 * Decodes every word of the table into its group, opcode, address, indirect
 * bit, micro-ops and shift count, and classifies the shape of the expression
 * the source wrote it as.  The decoder is deliberately dumb: it decodes the
 * bits the same way whether the word is an instruction, a pointer, a constant
 * or a packed character triple, and leaves the code/data judgment to
 * optrefs.c, for which the spelling is evidence.  Also holds the operate-group
 * phase table, the -O=decode dump and the -O=check self-check.
 *
 * Called from optimize() in optimizer.c; reads the parse tree without changing
 * it.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "optimizer.h"

// The operate-group phase table; the phases are explained at OptPhase in
// optimizer.h.  The handbook gives the shape (F-15D pages 21-22) and the
// emulator's cycle0() the time pulse of every micro-op, PDP-1D ones included:
//
//   TP7   cla, cli                 the clears
//   TP8   cmi, lat, lap, clf/stf   IO complement, ORs into AC, the flag field;
//                                  lai and lia are armed here
//   TP9   cma, hlt                 AC complement, then the stop
//   next  lia, lai                 the transfer completes in the next
//                                  instruction's fetch: lia alone is IO <- AC,
//                                  lai alone AC <- IO, both at once the exchange
//
// The order column separates rows within a phase only where it matters: cma
// before hlt for the listing, lia and lai equal because together they are one
// exchange.  clf and stf touch nothing another micro-op reads.
const OptMicroOp optMicroOps[OPTMO_COUNT] =
{
    { OPTMO_CLA, "cla", OPTM_CLA,       OPTPH_CLEAR,      0, OPTE_WRITES_AC,                   0 },
    { OPTMO_CLI, "cli", OPTM_CLI,       OPTPH_CLEAR,      0, OPTE_WRITES_IO,                   0 },
    { OPTMO_CLF, "clf", OPTM_FLAGFIELD, OPTPH_TRANSFER,   0, OPTE_WRITES_FLAGS,                0 },
    { OPTMO_STF, "stf", OPTM_FLAGFIELD, OPTPH_TRANSFER,   0, OPTE_WRITES_FLAGS,                0 },
    { OPTMO_LAT, "lat", OPTM_LAT,       OPTPH_TRANSFER,   0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_LAP, "lap", OPTM_LAP,       OPTPH_TRANSFER,   0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_CMA, "cma", OPTM_CMA,       OPTPH_COMPLEMENT, 0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_HLT, "hlt", OPTM_HLT,       OPTPH_COMPLEMENT, 1, OPTE_HALTS,                       0 },
    { OPTMO_CMI, "cmi", OPTM_CMI,       OPTPH_TRANSFER,   0, (OPTE_READS_IO | OPTE_WRITES_IO), 1 },
    { OPTMO_LIA, "lia", OPTM_LIA,       OPTPH_EXCHANGE,   0, (OPTE_READS_AC | OPTE_WRITES_IO), 1 },
    { OPTMO_LAI, "lai", OPTM_LAI,       OPTPH_EXCHANGE,   0, (OPTE_READS_IO | OPTE_WRITES_AC), 1 }
};

// Memory reference mnemonics indexed by the instruction code with bit 5
// dropped (code >> 1, so and = 02 is index 1); values from permsyms.def, which
// the handbook's page 67 list agrees with.  Code 12 is spare.  cal with bit 5
// set is jda, whose indirect bit does not indirect (page 18), as
// optDecodeValue() handles.  Spare codes and the group codes are NILP.
static const char *memrefNames[32] =
{
    NILP,  "and", "ior", "xor", "xct", NILP,  NILP,  "cal",    // codes 00 to 16
    "lac", "lio", "dac", "dap", "dip", "dio", "dzm", NILP,     // codes 20 to 36
    "add", "sub", "idx", "isp", "sad", "sas", "mul", "div",    // codes 40 to 56
    "jmp", "jsp", NILP,  NILP,  NILP,  NILP,  NILP,  NILP      // codes 60 to 76
};

// Shift group mnemonics, indexed [shift][right][registers] (handbook page
// 19).  A word with no register selected has no mnemonic; the handbook's
// group name sft stands in.
static const char *shiftNames[2][2][4] =
{
    { { "sft", "ral", "ril", "rcl" }, { "sft", "rar", "rir", "rcr" } },
    { { "sft", "sal", "sil", "scl" }, { "sft", "sar", "sir", "scr" } }
};

// The single-bit skip conditions in the order the dump prints them, highest
// bit first.
static const struct
{
    unsigned int bit;
    const char *nameP;
} skipCondNames[] =
{
    { OPTC_SNI, "sni" },
    { OPTC_SPI, "spi" },
    { OPTC_SZO, "szo" },
    { OPTC_SMA, "sma" },
    { OPTC_SPA, "spa" },
    { OPTC_SZA, "sza" }
};

// The special operate (code 74) micro-ops in the order the dump prints them.
static const struct
{
    unsigned int bit;
    const char *nameP;
} specialNames[] =
{
    { OPTX_IIF, "iif" },
    { OPTX_IFI, "ifi" },
    { OPTX_IDA, "ida" },
    { OPTX_SCI, "sci" },
    { OPTX_SCF, "scf" }
};

// The operate micro-ops in the order the dump prints them, highest bit
// first; the flag field is printed after these.
static const OptMicroId operateDumpOrder[] =
{
    OPTMO_CMI, OPTMO_CLI, OPTMO_LAT, OPTMO_CMA, OPTMO_HLT, OPTMO_CLA, OPTMO_LAP, OPTMO_LAI, OPTMO_LIA
};

// What the spelling scanner counts while it walks one word's expression.
typedef struct
{
    int lawCount;           // LAW nodes
    int opaddrCount;        // OPADDR nodes: mnemonics that take an address
    int opcodeCount;        // OPCODE and OPORABLE nodes: mnemonics that do not
    int oneDCount;          // of those, symbols flagged SYMF_1DOP
    int imodCount;          // i
    int modifierCount;      // VALUESPEC: 1s to 9s, C
    int numberCount;        // integers, characters, flexo codes, '.'
    int addrSymCount;       // ADDR, LCLADDR, BREF, WILDREF
    int constRefCount;      // [..]
    int arithCount;         // arithmetic operators at the top level of the word
    int otherCount;         // node types the scanner does not know
    int opInArith;          // an opcode-class symbol inside arithmetic
    SymNodeP firstOpSymP;   // the first opcode-class symbol in source order
} Shape, *ShapeP;

extern SymNodeP permSymP;   // the permanent symbols, from permsyms.def.c

static int bitCount(unsigned int bits);
static const char *findPermOpcodeName(SymNodeP symP, int value);
static void scanShape(PNodeP nodeP, ShapeP shapeP, int inArith);
static void noteOpSym(ShapeP shapeP, SymNodeP symP);
static void printGroup(FILE *fP, OptDecodeP decodeP);
static void printSpellingNames(FILE *fP, PNodeP nodeP);
static void checkResult(FILE *fP, const char *whatP, int passed, int *passedP, int *totalP);

// Decode one 18-bit value.  Every group's fields are filled from the bits
// alone; nothing here looks at the source.
void
optDecodeValue(int value, OptDecodeP decodeP)
{
int op6;
int i;

    memset(decodeP, 0, sizeof(OptDecode));
    value = (value & WRDMASK);

    // The six-bit code as the handbook writes it (two octal digits), split
    // into the five-bit instruction code and bit 5.
    op6 = ((value >> 12) & 077);
    decodeP->opcode = (op6 & 076);
    decodeP->indirect = (op6 & 01);
    decodeP->address = (value & ADDRMASK);

    switch( decodeP->opcode )
    {
    case 064:
        // Skip group: the single-bit conditions, the switch and flag
        // selectors, and bit 5 reversing the sense to "do not skip if".
        decodeP->group = OPTG_SKIP;
        decodeP->skipConds = (decodeP->address & OPTC_CONDMASK);
        decodeP->skipSwitch = ((decodeP->address & OPTC_SZSMASK) >> 3);
        decodeP->skipFlag = (decodeP->address & OPTC_SZFMASK);
        decodeP->skipInverted = decodeP->indirect;
        decodeP->uses1D = ((decodeP->skipConds & OPTC_SNI) != 0);
        break;

    case 066:
        // Shift group: bit 5 direction, bit 6 shift against rotate, bits 7-8
        // the registers, the count is the number of ones in bits 9-17.
        decodeP->group = OPTG_SHIFT;
        decodeP->shiftRight = decodeP->indirect;
        decodeP->shiftArith = ((decodeP->address & OPTSH_ARITH) != 0);
        decodeP->shiftRegs = ((decodeP->address & OPTSH_REGS) >> 9);
        decodeP->shiftCount = bitCount(decodeP->address & OPTSH_COUNT);
        decodeP->mnemonicP = shiftNames[decodeP->shiftArith][decodeP->shiftRight][decodeP->shiftRegs];
        break;

    case 070:
        // law N; bit 5 loads -N instead (handbook page 18).
        decodeP->group = OPTG_LAW;
        decodeP->mnemonicP = "law";
        break;

    case 072:
        // In-out transfer.  Named only when the whole value is one of the
        // iot mnemonics permsyms.def defines; otherwise it is just "iot".
        decodeP->group = OPTG_IOT;
        decodeP->iotWait = decodeP->indirect;
        decodeP->iotComplete = ((decodeP->address & OPTIO_COMPLETE) != 0);
        decodeP->iotSub = ((decodeP->address & OPTIO_SUBMASK) >> 6);
        decodeP->iotDevice = (decodeP->address & OPTIO_DEVMASK);
        decodeP->mnemonicP = findPermOpcodeName(permSymP, value);

        if( !decodeP->mnemonicP )
        {
            decodeP->mnemonicP = "iot";
        }
        break;

    case 074:
        // The PDP-1D special operate group; a spare (halting) code on a
        // plain PDP-1, but the 1D decode is always on.
        decodeP->group = OPTG_1D;
        decodeP->specialBits = (decodeP->address & OPTX_ALLBITS);
        decodeP->uses1D = 1;
        break;

    case 076:
        // Operate group: the micro-op bits are the word's low 13 bits.
        decodeP->group = OPTG_OPERATE;
        decodeP->microBits = (value & OPTM_ALLBITS);
        decodeP->flagNum = (decodeP->microBits & OPTM_FLAGNUM);

        if( decodeP->microBits & OPTM_STF )
        {
            decodeP->flagOp = OPTFLAG_STF;
        }
        else if( decodeP->flagNum )
        {
            decodeP->flagOp = OPTFLAG_CLF;
        }
        else
        {
            decodeP->flagOp = OPTFLAG_NONE;
        }

        decodeP->uses1D = ((decodeP->microBits & (OPTM_CMI | OPTM_LIA | OPTM_LAI)) != 0);
        break;

    default:
        // A memory reference, or a spare code.
        i = (decodeP->opcode >> 1);

        if( memrefNames[i] )
        {
            decodeP->group = OPTG_MEMREF;
            decodeP->mnemonicP = memrefNames[i];
            decodeP->memIndirect = decodeP->indirect;

            // cal with bit 5 set is jda, and jda never indirects.
            if( (decodeP->opcode == 016) && decodeP->indirect )
            {
                decodeP->mnemonicP = "jda";
                decodeP->memIndirect = 0;
            }
        }
        else
        {
            decodeP->group = OPTG_UNKNOWN;
        }
        break;
    }
}

// Classify how an expression tree spells its word, from one scan that counts
// its kinds of leaves and notes any opcode-class symbol inside arithmetic.
void
optClassifySpelling(PNodeP exprP, OptDecodeP decodeP)
{
Shape shape;
int operandCount;

    decodeP->spelling = OPTS_NONE;
    decodeP->spelled1D = 0;
    decodeP->spelledIndirect = 0;
    decodeP->hasConstRef = 0;
    decodeP->opSymCount = 0;
    decodeP->opSymP = NILP;

    if( !exprP )
    {
        return;
    }

    memset(&shape, 0, sizeof(shape));
    scanShape(exprP, &shape, 0);

    decodeP->spelled1D = (shape.oneDCount > 0);
    decodeP->spelledIndirect = (shape.imodCount > 0);
    decodeP->hasConstRef = (shape.constRefCount > 0);
    decodeP->opSymCount = (shape.lawCount + shape.opaddrCount + shape.opcodeCount);
    decodeP->opSymP = shape.firstOpSymP;

    // Anything that is not a mnemonic or a modifier counts as an operand.
    operandCount = (shape.numberCount + shape.addrSymCount + shape.arithCount + shape.constRefCount);

    // The rules, in order:
    //
    //   a [..] anywhere                          -> CONSTREF
    //   an opcode symbol inside arithmetic       -> OTHER   (jmp+foo)
    //   law, alone or with operands              -> LAW     (but law with another mnemonic is OTHER)
    //   no opcode symbol at all                  -> DATA
    //   one address-taking mnemonic (OPADDR):
    //       with any operand                     -> MNEMONIC_OPERAND   (lac x, jmp .+2, dac i p)
    //       with none, or only i                 -> MNEMONIC_ONLY      (cal, lac)
    //       with another mnemonic                -> OTHER              (jmp x cla, lac dac)
    //   only OPCODE/OPORABLE mnemonics:
    //       with no address symbol               -> MNEMONIC_ONLY      (sza i, cla cma, ral 3s, szf 7, opr 3200)
    //       with an address symbol               -> OTHER              (sza x)
    //   anything the scanner did not recognize   -> OTHER
    //
    // A modifier (i, 1s to 9s, C) is never an operand, and "ral 3" is
    // MNEMONIC_ONLY too because ral is an OPCODE, not an OPADDR.
    if( shape.constRefCount )
    {
        decodeP->spelling = OPTS_CONSTREF;
    }
    else if( shape.opInArith || shape.otherCount )
    {
        decodeP->spelling = OPTS_OTHER;
    }
    else if( shape.lawCount )
    {
        if( (shape.lawCount > 1) || shape.opaddrCount || shape.opcodeCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else
        {
            decodeP->spelling = OPTS_LAW;
        }
    }
    else if( !shape.opaddrCount && !shape.opcodeCount )
    {
        decodeP->spelling = OPTS_DATA;
    }
    else if( shape.opaddrCount )
    {
        if( (shape.opaddrCount > 1) || shape.opcodeCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else if( operandCount )
        {
            decodeP->spelling = OPTS_MNEMONIC_OPERAND;
        }
        else
        {
            decodeP->spelling = OPTS_MNEMONIC_ONLY;
        }
    }
    else
    {
        if( shape.addrSymCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else
        {
            decodeP->spelling = OPTS_MNEMONIC_ONLY;
        }
    }
}

// Decode one table entry: the value, then the spelling.  A reserved table word
// has no value; it is left OPTG_UNKNOWN with OPTS_NONE.
void
optDecodeWord(OptWordP entryP)
{
    if( entryP->flags & OPTF_RESERVED )
    {
        memset(&entryP->decode, 0, sizeof(OptDecode));
        return;
    }

    optDecodeValue(entryP->value, &entryP->decode);
    optClassifySpelling(entryP->exprP, &entryP->decode);
}

// Decode every entry of a table.
void
optDecodeTable(OptTableP tableP)
{
int i;

    for( i = 0; i < tableP->count; ++i )
    {
        optDecodeWord(tableP->entriesPP[i]);
    }
}

// Name a group for the report and the dump.
const char *
optGroupName(OptGroup group)
{
    switch( group )
    {
    case OPTG_MEMREF:
        return("memref");

    case OPTG_LAW:
        return("law");

    case OPTG_SKIP:
        return("skip");

    case OPTG_SHIFT:
        return("shift");

    case OPTG_OPERATE:
        return("operate");

    case OPTG_IOT:
        return("iot");

    case OPTG_1D:
        return("1d");

    case OPTG_UNKNOWN:
    default:
        return("unknown");
    }
}

// Name a spelling for the report and the dump.
const char *
optSpellingName(OptSpelling spelling)
{
    switch( spelling )
    {
    case OPTS_MNEMONIC_OPERAND:
        return("mnemonic_operand");

    case OPTS_MNEMONIC_ONLY:
        return("mnemonic_only");

    case OPTS_LAW:
        return("law");

    case OPTS_CONSTREF:
        return("constref");

    case OPTS_DATA:
        return("data");

    case OPTS_OTHER:
        return("other");

    case OPTS_NONE:
    default:
        return("none");
    }
}

// Print the decoded dump (-O=decode), one line per entry in emission order,
// reserved words included.
void
optDumpDecoded(FILE *fP, OptTableP tableP)
{
int i;
OptWordP entryP;

    // BBAAAA VVVVVV kind    group and fields | spelling names [1d]
    //
    // BBAAAA is the bank and address as in -T, VVVVVV the value (------ for a
    // reserved word), kind the OptKind name padded to seven characters.  The
    // group text is:
    //
    //   memref MNEM [i] AAAA     i when the word really indirects (jda never does)
    //   law [i] NNNN             i when the immediate is negated
    //   skip COND... [szsN] [szfN] [i]
    //                            conditions highest bit first, the selectors,
    //                            then i for a reversed sense; "skip skp" for none
    //   shift MNEM N             sft when no register is selected; N the count
    //   operate OP... [clfN|stfN]
    //                            micro-ops highest bit first, then the flag
    //                            operation; "operate nop" when no bit is set
    //   iot NAME [wait] [cpl] [sub NN] dev NN
    //                            the permsyms.def name when the value is exactly
    //                            one, else iot; bit 5, bit 6, bits 7-11 when
    //                            non-zero, and the device
    //   1d OP...                 iif ifi ida sci scf, or "1d none"
    //   unknown CC               the spare instruction code
    //   reserved                 a table word with no value
    //
    // After the bar: the spelling name, the opcode-class symbols and any i the
    // source wrote, in source order, then "1d" when a PDP-1D mnemonic was used.
    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        fprintf(fP, "%02o%04o ", entryP->bank, entryP->addr);

        if( entryP->flags & OPTF_RESERVED )
        {
            fprintf(fP, "------ %-7s reserved", kindName(entryP->kind));
        }
        else
        {
            fprintf(fP, "%06o %-7s ", entryP->value, kindName(entryP->kind));
            printGroup(fP, &entryP->decode);
        }

        fprintf(fP, " | %s", optSpellingName(entryP->decode.spelling));

        if( !(entryP->flags & OPTF_RESERVED) )
        {
            printSpellingNames(fP, entryP->exprP);
        }

        if( entryP->decode.spelled1D )
        {
            fprintf(fP, " 1d");
        }

        fprintf(fP, "\n");
    }
}

// Is a micro-op present in a set of operate-group bits?  The flag field is one
// slot: non-zero with bit 010 clear is clf, with it set stf.
// Returns 1 if present, 0 if not.
int
optMicroOpPresent(unsigned int microBits, OptMicroId id)
{
    switch( id )
    {
    case OPTMO_CLF:
        return( ((microBits & OPTM_FLAGFIELD) != 0) && ((microBits & OPTM_STF) == 0) );

    case OPTMO_STF:
        return( (microBits & OPTM_STF) != 0 );

    default:
        return( (microBits & optMicroOps[id].bits) != 0 );
    }
}

// List the micro-ops in microBits in hardware order: by phase, then the order
// column, then table position (an insertion sort over at most eleven rows).
// Returns how many pointers were stored in opsPP, at most max.
int
optOperateOrder(unsigned int microBits, const OptMicroOp **opsPP, int max)
{
int i;
int j;
int n;
const OptMicroOp *opP;

    n = 0;

    for( i = 0; (i < OPTMO_COUNT) && (n < max); ++i )
    {
        if( !optMicroOpPresent(microBits, (OptMicroId)i) )
        {
            continue;
        }

        opP = &optMicroOps[i];

        // Slide the new row back past every row that comes later than it.
        for( j = n; j > 0; --j )
        {
            if( (opsPP[j - 1]->phase < opP->phase) ||
                ((opsPP[j - 1]->phase == opP->phase) && (opsPP[j - 1]->order <= opP->order)) )
            {
                break;
            }

            opsPP[j] = opsPP[j - 1];
        }

        opsPP[j] = opP;
        ++n;
    }

    return(n);
}

// Apply an operate word's register micro-ops to AC and IO in phase order; lat
// ORs in tw and lap ORs in pc.  The sanity anchor for the phase table: lia or
// lai sees the register after the word's clears and complements.
void
optSimulateOperate(unsigned int microBits, int ac, int io, int tw, int pc, int *acP, int *ioP)
{
const OptMicroOp *opsP[OPTMO_COUNT];
int n;
int i;
int temp;
int swap;

    n = optOperateOrder(microBits, opsP, OPTMO_COUNT);
    swap = ((microBits & OPTM_LIA) && (microBits & OPTM_LAI));

    for( i = 0; i < n; ++i )
    {
        switch( opsP[i]->id )
        {
        case OPTMO_CLA:
            ac = 0;
            break;

        case OPTMO_CLI:
            io = 0;
            break;

        case OPTMO_LAT:
            ac = (ac | tw);
            break;

        case OPTMO_LAP:
            ac = (ac | pc);
            break;

        case OPTMO_CMA:
            ac = (~ac);
            break;

        case OPTMO_CMI:
            io = (~io);
            break;

        case OPTMO_LIA:
            if( swap )
            {
                // The exchange: both transfers at once; lai is then a no-op below.
                temp = ac;
                ac = io;
                io = temp;
            }
            else
            {
                io = ac;
            }
            break;

        case OPTMO_LAI:
            if( !swap )
            {
                ac = io;
            }
            break;

        case OPTMO_CLF:
        case OPTMO_STF:
        case OPTMO_HLT:
        default:
            break;
        }
    }

    *acP = (ac & WRDMASK);
    *ioP = (io & WRDMASK);
}

// The decoder's self-check (-O=check): the phase-table anchors, then decodes
// worked out by hand from permsyms.def and the handbook, one line each to fP.
// Returns 1 if every check passed, 0 if any failed.
int
optDecoderSelfCheck(FILE *fP)
{
int passed;
int total;
int ac;
int io;
int n;
const OptMicroOp *opsP[OPTMO_COUNT];
OptDecode d;

    passed = 0;
    total = 0;

    // Phase-table anchors.

    // cla cma in one word, 761200: the clear is applied before the
    // complement, so the result is all ones whatever AC held.
    optDecodeValue(0761200, &d);
    checkResult(fP, "761200 is operate cla cma", (d.group == OPTG_OPERATE) && (d.microBits == (OPTM_CLA | OPTM_CMA)),
        &passed, &total);
    n = optOperateOrder(d.microBits, opsP, OPTMO_COUNT);
    checkResult(fP, "761200 applies cla then cma", (n == 2) && (opsP[0]->id == OPTMO_CLA) && (opsP[1]->id == OPTMO_CMA),
        &passed, &total);
    optSimulateOperate(d.microBits, 0, 0, 0, 0, &ac, &io);
    checkResult(fP, "761200 from AC=0 gives 777777", (ac == 0777777), &passed, &total);
    optSimulateOperate(d.microBits, 0123456, 0, 0, 0, &ac, &io);
    checkResult(fP, "761200 from AC=123456 gives 777777", (ac == 0777777), &passed, &total);

    // cma then cla as two words: complement first, then clear, so zero.
    optSimulateOperate(OPTM_CMA, 0, 0, 0, 0, &ac, &io);
    checkResult(fP, "761000 (cma) from AC=0 gives 777777", (ac == 0777777), &passed, &total);
    optSimulateOperate(OPTM_CLA, ac, 0, 0, 0, &ac, &io);
    checkResult(fP, "then 760200 (cla) gives 0", (ac == 0), &passed, &total);

    // The handbook's own example, page 21: opr 3200 clears AC, ORs in the
    // test word, complements.  With TW = 123 that is the complement of 123.
    optSimulateOperate(03200, 0777777, 0, 0123, 0, &ac, &io);
    checkResult(fP, "763200 (opr 3200) with TW=123 gives 777654", (ac == 0777654), &passed, &total);

    // lat as permsyms.def spells it, 762200, loads exactly the test word.
    optSimulateOperate(02200, 0777777, 0, 0123, 0, &ac, &io);
    checkResult(fP, "762200 (lat) with TW=123 gives 123", (ac == 0123), &passed, &total);

    // cli cla clears both registers.
    optSimulateOperate((OPTM_CLI | OPTM_CLA), 5, 7, 0, 0, &ac, &io);
    checkResult(fP, "765200 (cli cla) clears AC and IO", (ac == 0) && (io == 0), &passed, &total);

    // lsw/swp is an exchange.
    optSimulateOperate(060, 1, 2, 0, 0, &ac, &io);
    checkResult(fP, "760060 (swp) exchanges AC and IO", (ac == 2) && (io == 1), &passed, &total);

    // The PDP-1D transfers complete after everything else in the word (armed
    // at TP8, done in the next fetch), so they see the cleared and
    // complemented registers.
    optSimulateOperate((OPTM_CLA | OPTM_LIA), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "760220 (cla lia) gives AC=0 IO=0: lia copies the cleared AC", (ac == 0) && (io == 0), &passed, &total);
    optSimulateOperate((OPTM_CMI | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "770040 (cmi lai) gives AC=777654: lai copies the complemented IO", (ac == 0777654) && (io == 0777654),
        &passed, &total);
    optSimulateOperate((OPTM_CMA | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "761040 (cma lai) gives AC=123: the complement is overwritten by the transfer", (ac == 0123),
        &passed, &total);
    optSimulateOperate((OPTM_CLI | OPTM_LIA | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "764060 (cli swp) gives AC=0 IO=777: the exchange sees the cleared IO", (ac == 0) && (io == 0777),
        &passed, &total);
    optSimulateOperate((OPTM_CLI | OPTM_CMI), 0, 0123, 0, 0, &ac, &io);
    checkResult(fP, "774000 (cli cmi) gives IO=777777: the clear comes before the complement", (io == 0777777),
        &passed, &total);

    // Hand-decoded words.

    optDecodeValue(0210004, &d);
    checkResult(fP, "210004 is memref lac i 0004",
        (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "lac") && d.indirect && d.memIndirect && (d.address == 04),
        &passed, &total);

    optDecodeValue(0170100, &d);
    checkResult(fP, "170100 is memref jda 0100, not indirect",
        (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "jda") && d.indirect && !d.memIndirect && (d.address == 0100),
        &passed, &total);

    optDecodeValue(0160000, &d);
    checkResult(fP, "160000 is memref cal", (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "cal") && !d.memIndirect,
        &passed, &total);

    optDecodeValue(0650100, &d);
    checkResult(fP, "650100 is skip sza i (do not skip on zero AC)",
        (d.group == OPTG_SKIP) && (d.skipConds == OPTC_SZA) && d.skipInverted && !d.skipSwitch && !d.skipFlag,
        &passed, &total);

    optDecodeValue(0640500, &d);
    checkResult(fP, "640500 is skip sma sza",
        (d.group == OPTG_SKIP) && (d.skipConds == (OPTC_SMA | OPTC_SZA)) && !d.skipInverted, &passed, &total);

    optDecodeValue(0640067, &d);
    checkResult(fP, "640067 is skip szs6 szf7",
        (d.group == OPTG_SKIP) && !d.skipConds && (d.skipSwitch == 6) && (d.skipFlag == 7), &passed, &total);

    optDecodeValue(0654000, &d);
    checkResult(fP, "654000 (szi) is skip sni i (skip if IO is zero), PDP-1D",
        (d.group == OPTG_SKIP) && (d.skipConds == OPTC_SNI) && d.skipInverted && !d.skipSwitch && !d.skipFlag &&
        d.uses1D, &passed, &total);

    optDecodeValue(0661007, &d);
    checkResult(fP, "661007 is shift ral 3",
        (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "ral") && (d.shiftCount == 3) && !d.shiftRight && !d.shiftArith &&
        (d.shiftRegs == 1), &passed, &total);

    optDecodeValue(0677777, &d);
    checkResult(fP, "677777 is shift scr 9",
        (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "scr") && (d.shiftCount == 9) && d.shiftRight && d.shiftArith &&
        (d.shiftRegs == 3), &passed, &total);

    optDecodeValue(0660000, &d);
    checkResult(fP, "660000 is shift sft 0", (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "sft") && (d.shiftCount == 0),
        &passed, &total);

    optDecodeValue(0710020, &d);
    checkResult(fP, "710020 is law i 0020", (d.group == OPTG_LAW) && d.indirect && (d.address == 020), &passed, &total);

    optDecodeValue(0730003, &d);
    checkResult(fP, "730003 is iot tyo wait dev 03",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "tyo") && d.iotWait && !d.iotComplete && !d.iotSub &&
        (d.iotDevice == 03), &passed, &total);

    optDecodeValue(0724074, &d);
    checkResult(fP, "724074 is iot eem cpl dev 74",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "eem") && !d.iotWait && d.iotComplete && (d.iotDevice == 074),
        &passed, &total);

    optDecodeValue(0722007, &d);
    checkResult(fP, "722007 is iot sdb sub 20 dev 07",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "sdb") && (d.iotSub == 020) && (d.iotDevice == 07), &passed, &total);

    optDecodeValue(0720123, &d);
    checkResult(fP, "720123 is an unnamed iot", (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "iot"), &passed, &total);

    optDecodeValue(0770000, &d);
    checkResult(fP, "770000 is operate cmi, PDP-1D",
        (d.group == OPTG_OPERATE) && (d.microBits == OPTM_CMI) && d.uses1D, &passed, &total);

    optDecodeValue(0760017, &d);
    checkResult(fP, "760017 is operate stf 7",
        (d.group == OPTG_OPERATE) && (d.flagOp == OPTFLAG_STF) && (d.flagNum == 7) && !d.uses1D, &passed, &total);

    optDecodeValue(0760003, &d);
    checkResult(fP, "760003 is operate clf 3",
        (d.group == OPTG_OPERATE) && (d.flagOp == OPTFLAG_CLF) && (d.flagNum == 3), &passed, &total);

    optDecodeValue(0760000, &d);
    checkResult(fP, "760000 is operate nop",
        (d.group == OPTG_OPERATE) && !d.microBits && (d.flagOp == OPTFLAG_NONE), &passed, &total);

    optDecodeValue(0744000, &d);
    checkResult(fP, "744000 is 1d iif", (d.group == OPTG_1D) && (d.specialBits == OPTX_IIF) && d.uses1D, &passed, &total);

    optDecodeValue(0000000, &d);
    checkResult(fP, "000000 is unknown 00", (d.group == OPTG_UNKNOWN) && (d.opcode == 0), &passed, &total);

    // Code 12 is spare, and an address field in the word must not change that.
    optDecodeValue(0120100, &d);
    checkResult(fP, "120100 is unknown 12", (d.group == OPTG_UNKNOWN) && (d.opcode == 012), &passed, &total);

    optDecodeValue(0140000, &d);
    checkResult(fP, "140000 is unknown 14", (d.group == OPTG_UNKNOWN) && (d.opcode == 014), &passed, &total);

    optDecodeValue(0360000, &d);
    checkResult(fP, "360000 is unknown 36", (d.group == OPTG_UNKNOWN) && (d.opcode == 036), &passed, &total);

    fprintf(fP, "decoder self-check: %d of %d checks passed\n", passed, total);
    return( passed == total );
}

// Count the one bits in a value.
static int
bitCount(unsigned int bits)
{
int n;

    for( n = 0; bits; bits >>= 1 )
    {
        n += (bits & 1);
    }

    return(n);
}

// Find the permanent opcode-type symbol with exactly this value, to name an
// iot word.  Returns the symbol's name, or NILP if no opcode symbol has it.
static const char *
findPermOpcodeName(SymNodeP symP, int value)
{
const char *nameP;

    if( !symP )
    {
        return(NILP);
    }

    if( ((symP->flags & SYM_MASK) == SYM_OPCODE) && (symP->value == value) )
    {
        return(symP->name);
    }

    if( (nameP = findPermOpcodeName(symP->leftP, value)) )
    {
        return(nameP);
    }

    return( findPermOpcodeName(symP->rightP, value) );
}

// Count what one word's expression is made of.  The separator and parentheses
// are transparent; any other operator is arithmetic (inArith).  A [..] is
// counted, not entered: its inside spells the pool word, not this one.
static void
scanShape(PNodeP nodeP, ShapeP shapeP, int inArith)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        if( nodeP->value.ival == SEPARATOR )
        {
            scanShape(nodeP->leftP, shapeP, inArith);
            scanShape(nodeP->rightP, shapeP, inArith);
        }
        else
        {
            if( !inArith )
            {
                ++shapeP->arithCount;
            }

            scanShape(nodeP->leftP, shapeP, 1);
            scanShape(nodeP->rightP, shapeP, 1);
        }
        break;

    case UNOP:
        if( nodeP->value.ival == PARENS )
        {
            scanShape(nodeP->rightP, shapeP, inArith);
        }
        else
        {
            if( !inArith )
            {
                ++shapeP->arithCount;
            }

            scanShape(nodeP->rightP, shapeP, 1);
        }
        break;

    case LAW:
        ++shapeP->lawCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;
        break;

    case OPADDR:
        ++shapeP->opaddrCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;
        break;

    case OPCODE:
    case OPORABLE:
        ++shapeP->opcodeCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;

        if( nodeP->value.symP && (nodeP->value.symP->flags & SYMF_1DOP) )
        {
            ++shapeP->oneDCount;
        }
        break;

    case IMOD:
        ++shapeP->imodCount;
        break;

    case VALUESPEC:
        ++shapeP->modifierCount;
        break;

    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
    case DOT:
        ++shapeP->numberCount;
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
    case WILDREF:
        ++shapeP->addrSymCount;
        break;

    case CONSTANT:
        ++shapeP->constRefCount;
        break;

    default:
        ++shapeP->otherCount;
        break;
    }
}

// Remember the first opcode-class symbol the scanner meets.
static void
noteOpSym(ShapeP shapeP, SymNodeP symP)
{
    if( !shapeP->firstOpSymP )
    {
        shapeP->firstOpSymP = symP;
    }
}

// Print the group and field text of one decoded word, as optDumpDecoded()
// describes.
static void
printGroup(FILE *fP, OptDecodeP decodeP)
{
int i;
int any;

    switch( decodeP->group )
    {
    case OPTG_MEMREF:
        fprintf(fP, "memref %s%s %04o", decodeP->mnemonicP, (decodeP->memIndirect)?" i":"", decodeP->address);
        break;

    case OPTG_LAW:
        fprintf(fP, "law%s %04o", (decodeP->indirect)?" i":"", decodeP->address);
        break;

    case OPTG_SKIP:
        fprintf(fP, "skip");
        any = 0;

        for( i = 0; i < (int)(sizeof(skipCondNames) / sizeof(skipCondNames[0])); ++i )
        {
            if( decodeP->skipConds & skipCondNames[i].bit )
            {
                fprintf(fP, " %s", skipCondNames[i].nameP);
                any = 1;
            }
        }

        if( decodeP->skipSwitch )
        {
            fprintf(fP, " szs%o", decodeP->skipSwitch);
            any = 1;
        }

        if( decodeP->skipFlag )
        {
            fprintf(fP, " szf%o", decodeP->skipFlag);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " skp");
        }

        if( decodeP->skipInverted )
        {
            fprintf(fP, " i");
        }
        break;

    case OPTG_SHIFT:
        fprintf(fP, "shift %s %d", decodeP->mnemonicP, decodeP->shiftCount);
        break;

    case OPTG_OPERATE:
        fprintf(fP, "operate");

        if( !decodeP->microBits )
        {
            fprintf(fP, " nop");
            break;
        }

        for( i = 0; i < (int)(sizeof(operateDumpOrder) / sizeof(operateDumpOrder[0])); ++i )
        {
            if( optMicroOpPresent(decodeP->microBits, operateDumpOrder[i]) )
            {
                fprintf(fP, " %s", optMicroOps[operateDumpOrder[i]].nameP);
            }
        }

        if( decodeP->flagOp == OPTFLAG_CLF )
        {
            fprintf(fP, " clf%o", decodeP->flagNum);
        }
        else if( decodeP->flagOp == OPTFLAG_STF )
        {
            fprintf(fP, " stf%o", decodeP->flagNum);
        }
        break;

    case OPTG_IOT:
        fprintf(fP, "iot %s", decodeP->mnemonicP);

        if( decodeP->iotWait )
        {
            fprintf(fP, " wait");
        }

        if( decodeP->iotComplete )
        {
            fprintf(fP, " cpl");
        }

        if( decodeP->iotSub )
        {
            fprintf(fP, " sub %02o", decodeP->iotSub);
        }

        fprintf(fP, " dev %02o", decodeP->iotDevice);
        break;

    case OPTG_1D:
        fprintf(fP, "1d");
        any = 0;

        for( i = 0; i < (int)(sizeof(specialNames) / sizeof(specialNames[0])); ++i )
        {
            if( decodeP->specialBits & specialNames[i].bit )
            {
                fprintf(fP, " %s", specialNames[i].nameP);
                any = 1;
            }
        }

        if( !any )
        {
            fprintf(fP, " none");
        }
        break;

    case OPTG_UNKNOWN:
    default:
        fprintf(fP, "unknown %02o", decodeP->opcode);
        break;
    }
}

// Print the opcode-class symbols and any i in a word's expression in source
// order, each after a space: what the source wrote, beside the decoded bits.
static void
printSpellingNames(FILE *fP, PNodeP nodeP)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        printSpellingNames(fP, nodeP->leftP);
        printSpellingNames(fP, nodeP->rightP);
        break;

    case UNOP:
        printSpellingNames(fP, nodeP->rightP);
        break;

    case LAW:
    case OPADDR:
    case OPCODE:
    case OPORABLE:
        if( nodeP->value.symP )
        {
            fprintf(fP, " %s", nodeP->value.symP->name);
        }
        break;

    case IMOD:
        fprintf(fP, " i");
        break;

    default:
        break;
    }
}

// Record and print one self-check result.
static void
checkResult(FILE *fP, const char *whatP, int passed, int *passedP, int *totalP)
{
    ++*totalP;

    if( passed )
    {
        ++*passedP;
        fprintf(fP, "  ok   %s\n", whatP);
    }
    else
    {
        fprintf(fP, "  FAIL %s\n", whatP);
    }
}

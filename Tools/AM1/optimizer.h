/* optimizer.h - the am1 optimizer's shared header
 *
 * Declares the word table every optimizer analysis is built on, the records
 * the analyses and rewrites keep beside it, and every entry point one opt*.c
 * file publishes to another; anything one file uses alone stays static there.
 * am1.c calls optimizeSetOption(), optimizeSetLevel() and optimize().
 *
 * The optimizer runs after yyparse() and before the code generators.  It reads
 * the parse tree and the symbol tables and keeps what it learns in side tables
 * keyed by the PNodeP or SymNodeP that produced each word.  Only the rewrite
 * (opttransform.c, under -O1 or -O2) and relayout (optrelayout.c) write parse
 * nodes, after every analysis has finished; no symbol is ever written.
 *
 * Requires am1.h (PNode, SymNode, BankContext, MAXBANK, BANKSIZE, WRDMASK) to be
 * included first.  Single threaded; one optimize() call per am1 run.
*/
#ifndef OPTIMIZER_H
#define OPTIMIZER_H

// A parse-node flag on a BREF that was written 'sym:*' and resolved to a bank
// reference: the constant pools key the two spellings differently.  Set by
// relayout and by srccodegen.c from the parser's list of wildcards, and carried
// by relayout's copies.  Above every PN_ flag in am1.h.
#define RL_WILD             0x20000000

// What produced a word.  EXPR is every single-word statement whose content is
// an expression tree (instruction, data word, law); the decoder tells them
// apart.  The other kinds are known from the emitting statement.
typedef enum
{
    OPTK_EXPR,          // an expression statement, with or without a label
    OPTK_TEXT,          // one word of a packed flexo 'text' string
    OPTK_ASCII,         // one word of a packed 'ascii' string
    OPTK_TYPE340,       // one word of a packed 'type340' string
    OPTK_TABLE,         // one word of a 'table', initialized or reserved
    OPTK_VAR,           // one variable's storage word
    OPTK_CONST          // one constant-pool word
} OptKind;

// Per-word flags.  The table builder owns the low byte; each analysis owns a
// field of its own above it.
#define OPTF_RESERVED       0x0001  // a 'table' word with no initializer: no value is emitted
#define OPTF_DUPADDR        0x0002  // another entry shares this bank and address (-M overlay)
#define OPTF_PCMISMATCH     0x0004  // node pc differs from the binary generator's: a parser defect
#define OPTF_BUILDER_MASK   0x00FF  // every flag the builder owns

// Reference flags, derived from the edges.  The MAYBE flags are conservative:
// an indirect reference whose pointer cannot be resolved may reach any taken
// word in its bank, so each such word is marked in the reference's role.
#define OPTF_WRITTEN        0x00000100  // a write edge comes in (dap and dip included)
#define OPTF_PATCHED        0x00000200  // a dap or dip edge comes in: the address field varies
#define OPTF_TAKEN          0x00000400  // a taken edge comes in: the address is used as a value
#define OPTF_XCTTARGET      0x00000800  // an execute edge comes in
#define OPTF_READ           0x00001000  // a read edge comes in
#define OPTF_JUMPTARGET     0x00002000  // a jump edge comes in
#define OPTF_START          0x00004000  // the word at the program's start address
#define OPTF_CODE           0x00008000  // classified code
#define OPTF_DATA           0x00010000  // classified data
#define OPTF_MAYBE_READ     0x00020000  // possibly read through an unresolved pointer
#define OPTF_MAYBE_WRITTEN  0x00040000  // possibly written through an unresolved pointer
#define OPTF_MAYBE_ENTERED  0x00080000  // possibly jumped to or executed through an unresolved pointer
#define OPTF_ANALYSIS_MASK  0x000FFF00  // every flag the reference analysis owns

// Control-flow flags.  HASLABEL and AFTERSKIP are set on every entry, in the
// graph or not, because P1 and P4 ask about a word's neighborhood, not its
// role; the other four only on words the graph holds.
#define OPTF_HASLABEL       0x00100000  // a label is defined here; distinct from a block
                                        // start, so P1 sees a label on an interior word
#define OPTF_AFTERSKIP      0x00200000  // the word below decodes as a skip, so this one
                                        // may be skipped over (P4)
#define OPTF_BLOCKSTART     0x00400000  // the first word of a basic block
#define OPTF_ENTRY          0x00800000  // an entry point of the reachability walk
#define OPTF_UNREACHED      0x01000000  // in a block no entry reaches, and not xct'd
#define OPTF_XCTONLY        0x02000000  // unreached, but executed in place by an xct
#define OPTF_RESUMED        0x04000000  // no entry reaches it, but a jmp or jsp names its
                                        // block: live code the walk lost.  Reporting only;
                                        // OPTF_UNREACHED is set too, and is what the rules
                                        // and heuristics ask about
#define OPTF_FLOW_MASK      0x0FF00000  // every flag the control-flow overlay owns

// Region flags, set by the table builder as each word is created, from a
// counter the directive nodes raise and lower, so a region that spans a bank
// change, an overlay or a macro needs no range arithmetic.  OPTF_INREGION is a
// permission to rewrite, not a refusal of anything outside it.
#define OPTF_INREGION       0x10000000  // emitted between an optimize and its endoptimize
#define OPTF_HANDSOFF       0x20000000  // between nooptimize and endnooptimize: never rewritten
#define OPTF_INSPEED        0x40000000  // between speed and endspeed: a length-changing
                                        // rewrite may spend words here.  Independent of
                                        // OPTF_INREGION
#define OPTF_REGION_MASK    0xF0000000  // every flag the region overlay owns

// Conflicts are recorded, never resolved: a transform must leave these words
// alone.  Patched implies written, so OPTCF_CODE_PATCHED implies
// OPTCF_CODE_WRITTEN.
#define OPTCF_CODE_DATA     0x01    // classified both code and data
#define OPTCF_CODE_PATCHED  0x02    // code whose address field is written by dap or dip
#define OPTCF_CODE_WRITTEN  0x04    // code that is written at all

// A label attached to a word: a LOCATION or LCLLOCATION label, a variable name
// or a constant-pool entry.  The list is owned by the word it hangs from.
typedef struct optlabel
{
    struct optlabel *nextP;
    SymNodeP symP;
} OptLabel, *OptLabelP;

// Instruction decode and source spelling.
//
// Every word with a value gets a decoded view of its bits: what they would do
// if fetched as an instruction, whatever the word was meant to be.  Bits are
// numbered as in the F-15D handbook, page 9: bit 0 is the most significant,
// bits 0-4 the instruction code, bit 5 (010000) the indirect bit or a group
// modifier, bits 6-17 (07777) the address.  Mnemonic values are permsyms.def's.

// The group the instruction code selects.  Codes 00, 12, 14 and 36 are spare
// (handbook page 66).  Code 74 is the PDP-1D special operate group; the 1D
// decode is always on, so 74 is always OPTG_1D.
typedef enum
{
    OPTG_UNKNOWN,   // a spare instruction code
    OPTG_MEMREF,    // memory reference: and ior xor xct cal jda lac lio dac dap
                    // dip dio dzm add sub idx isp sad sas mul div jmp jsp
    OPTG_LAW,       // law N, code 70; bit 5 negates N
    OPTG_SKIP,      // the skip group, code 64; bit 5 reverses the sense
    OPTG_SHIFT,     // the shift group, code 66; bit 5 set (67) shifts right
    OPTG_OPERATE,   // the operate group, code 76; bit 5 set (77) is the PDP-1D cmi
    OPTG_IOT,       // in-out transfer, code 72; bit 5 set (73) waits for completion
    OPTG_1D         // the PDP-1D special operate group, code 74
} OptGroup;

// How the source spelled the word, from its expression tree.  Evidence for the
// code/data classification, not a verdict: a dispatch table of "jmp foo" words
// is spelled MNEMONIC_OPERAND and is still data.
typedef enum
{
    OPTS_NONE,              // no expression tree: text, ascii, type340 words,
                            // reserved table words, variables without an initializer
    OPTS_MNEMONIC_OPERAND,  // one address-taking mnemonic with an operand:
                            // lac x, jmp .+2, dac i p, add [5] is CONSTREF instead
    OPTS_MNEMONIC_ONLY,     // mnemonics and modifiers with no address symbol:
                            // sza, sza i, cla cma, ral 3s, tyo, szf 7, opr 3200,
                            // and a bare address-taking mnemonic such as cal
    OPTS_LAW,               // law, with or without an operand
    OPTS_CONSTREF,          // a [..] constant reference anywhere in the word
    OPTS_DATA,              // no opcode symbol at all: a number, an address
                            // symbol, a character, arithmetic over those, a
                            // bare modifier such as i or 3s
    OPTS_OTHER              // anything else: a mnemonic inside arithmetic
                            // (jmp+foo), two address-taking mnemonics, a
                            // mnemonic mixed with an operate micro-op (jmp x cla),
                            // a skip or shift with an address symbol (sza x)
} OptSpelling;

// Skip group condition bits, bits 6-17 of a code 64 word (handbook pages
// 20-21).  Combined conditions skip if ANY holds; bit 5 inverts the whole
// sense.  The switch and flag fields select 1-6, or 7 for all.  szi (654000)
// is sni with bit 5 set, so it skips when IO is zero.
#define OPTC_SNI        0004000     // PDP-1D: skip if IO is not zero
#define OPTC_SPI        0002000     // skip if IO is positive (sign bit zero)
#define OPTC_SZO        0001000     // skip if the overflow flip-flop is zero
#define OPTC_SMA        0000400     // skip if AC is negative (sign bit one)
#define OPTC_SPA        0000200     // skip if AC is positive (sign bit zero)
#define OPTC_SZA        0000100     // skip if AC is plus zero
#define OPTC_CONDMASK   0007700     // the single-bit conditions above
#define OPTC_SZSMASK    0000070     // sense switch number, 1 to 6, 7 = all
#define OPTC_SZFMASK    0000007     // program flag number, 1 to 6, 7 = all

// Shift group fields, code 66 (handbook pages 9, 18-19).  Bit 5 is the
// direction; the step count is the number of one bits in bits 9-17
// ("rar 9 = 671777").
#define OPTSH_ARITH     0004000     // bit 6: one = shift, zero = rotate
#define OPTSH_REGS      0003000     // bits 7 and 8: 01 = AC, 10 = IO, 11 = both
#define OPTSH_AC        0001000
#define OPTSH_IO        0002000
#define OPTSH_COUNT     0000777     // bits 9 to 17, one bit per step

// Operate group micro-op bits, bits 5-17 of a code 76 word (handbook pages
// 21-22; the PDP-1D bits from permsyms.def).  These are the hardware bits.
// lat (762200) and lap (760300) carry cla as well.  The flag field is one slot:
// 0001-0007 clear flag n, 0011-0017 set it, never both in one word.
#define OPTM_CMI        0010000     // PDP-1D: complement IO
#define OPTM_CLI        0004000     // clear IO
#define OPTM_LAT        0002000     // OR the test word switches into AC
#define OPTM_CMA        0001000     // complement AC
#define OPTM_HLT        0000400     // halt
#define OPTM_CLA        0000200     // clear AC
#define OPTM_LAP        0000100     // OR the program counter (and overflow) into AC
#define OPTM_LAI        0000040     // PDP-1D: load AC from IO
#define OPTM_LIA        0000020     // PDP-1D: load IO from AC
#define OPTM_STF        0000010     // flag field: one = set, zero = clear
#define OPTM_FLAGNUM    0000007     // flag field: flag number, 7 = all
#define OPTM_FLAGFIELD  0000017     // the whole flag field
#define OPTM_ALLBITS    0017777     // every micro-op bit

// Special operate group bits, bits 6-17 of a code 74 word (permsyms.def and
// Docs/UsingPDP-1DInstructions.md).
#define OPTX_IIF        0004000     // OR IO bits 12-17 with the program flags, into IO
#define OPTX_IFI        0002000     // OR the program flags with IO bits 12-17, into the flags
#define OPTX_IDA        0000400     // add one to AC
#define OPTX_SCI        0000100     // clear IO
#define OPTX_SCF        0000040     // clear program flags 1 to 6
#define OPTX_ALLBITS    0006540

// In-out transfer fields (handbook page 22).  Bit 5 waits for completion; a
// completion pulse comes when bit 6 differs from bit 5.  Otherwise an iot is
// opaque: named only when its value is exactly a permsyms.def mnemonic.
#define OPTIO_COMPLETE  0004000     // bit 6
#define OPTIO_SUBMASK   0003700     // bits 7 to 11
#define OPTIO_DEVMASK   0000077     // bits 12 to 17

// What the flag field of an operate word does.
typedef enum
{
    OPTFLAG_NONE,           // the field is zero
    OPTFLAG_CLF,            // clear flag flagNum (7 = all)
    OPTFLAG_STF             // set flag flagNum (7 = all)
} OptFlagOp;

// The hardware phase an operate micro-op acts in (handbook page 21, and the
// emulator's timing).  Clears act at TP7; complement IO, the ORs into AC and
// the flag operation at TP8; complement AC and halt at TP9.  lai and lia are armed at TP8 and
// complete during the NEXT fetch, as one exchange when both are set, so they
// read their source after cla, cli, cmi and cma.  Hence cla cma in one word
// (clc) leaves all ones, while cma then cla as two words leaves zero.
typedef enum
{
    OPTPH_CLEAR = 1,        // TP7: cla, cli
    OPTPH_TRANSFER,         // TP8: cmi, lat, lap, clf, stf
    OPTPH_COMPLEMENT,       // TP9: cma, hlt
    OPTPH_EXCHANGE          // next fetch: lia, lai (together, the swap)
} OptPhase;

// The micro-ops, in the order the table optMicroOps[] lists them.
typedef enum
{
    OPTMO_CLA,
    OPTMO_CLI,
    OPTMO_CLF,
    OPTMO_STF,
    OPTMO_LAT,
    OPTMO_LAP,
    OPTMO_CMA,
    OPTMO_HLT,
    OPTMO_CMI,
    OPTMO_LIA,
    OPTMO_LAI,
    OPTMO_COUNT
} OptMicroId;

// What a micro-op reads and writes, for the merge rules.
#define OPTE_READS_AC       0x01
#define OPTE_WRITES_AC      0x02
#define OPTE_READS_IO       0x04
#define OPTE_WRITES_IO      0x08
#define OPTE_WRITES_FLAGS   0x10
#define OPTE_HALTS          0x20

// One row of the operate-group phase table.
typedef struct
{
    OptMicroId id;
    const char *nameP;      // the mnemonic
    unsigned int bits;      // the bits that select it; for clf and stf the whole
                            // flag field, decoded by optMicroOpPresent()
    OptPhase phase;         // when the hardware applies it
    int order;              // order within the phase, lower first; equal means
                            // simultaneous, or that no order matters
    unsigned int effects;   // OPTE_ bits
    int isOneD;             // non-zero for a PDP-1D micro-op
} OptMicroOp;

extern const OptMicroOp optMicroOps[OPTMO_COUNT];

// The decoded view of one word.  Every field is filled from the value alone
// by optDecodeValue(); the spelling fields are filled from the expression
// tree by optClassifySpelling().  Fields that do not apply to the group are
// zero.
typedef struct optdecode
{
    OptGroup group;
    int opcode;             // bits 0-4 as the handbook writes them: two octal
                            // digits with bit 5 clear, 020 for lac, 076 for opr
    int indirect;           // bit 5, raw, whatever it means to the group
    int address;            // bits 6-17, raw
    const char *mnemonicP;  // the handbook mnemonic for a memory reference, law,
                            // shift or iot word; NILP for the groups whose word
                            // is a set of conditions or micro-ops, and for unknown
    int uses1D;             // the word carries bits only a PDP-1D acts on

    // memory reference group
    int memIndirect;        // the word really indirects: bit 5 set and the
                            // word is not jda, whose bit 5 is part of the code

    // skip group
    unsigned int skipConds; // OPTC_ single-bit conditions present
    int skipSwitch;         // sense switch field, 0 none, 1-6, 7 all
    int skipFlag;           // program flag field, 0 none, 1-6, 7 all
    int skipInverted;       // bit 5: do NOT skip when the condition holds

    // shift group
    int shiftRight;         // bit 5: one = right, zero = left
    int shiftArith;         // bit 6: one = shift, zero = rotate
    int shiftRegs;          // bits 7-8: 1 AC, 2 IO, 3 both, 0 neither
    int shiftCount;         // number of one bits in bits 9-17

    // operate group
    unsigned int microBits; // (value & OPTM_ALLBITS)
    OptFlagOp flagOp;       // what the flag field does
    int flagNum;            // the flag it does it to, 7 = all

    // special operate group (PDP-1D)
    unsigned int specialBits;   // (value & OPTX_ALLBITS)

    // in-out transfer
    int iotWait;            // bit 5
    int iotComplete;        // bit 6
    int iotSub;             // bits 7-11
    int iotDevice;          // bits 12-17

    // spelling, from the expression tree
    OptSpelling spelling;
    int spelled1D;          // the source used a PDP-1D mnemonic
    int spelledIndirect;    // the source wrote i
    int hasConstRef;        // the source has a [..] somewhere in the word
    int opSymCount;         // opcode-class symbols (OPADDR, OPCODE, OPORABLE, law) in the word
    SymNodeP opSymP;        // the first of them in source order, NILP if none
} OptDecode, *OptDecodeP;

// Reference edges.
//
// Every symbol a word names becomes an edge to the word(s) at its address,
// with a role.  In a memory reference's address field the role is the
// mnemonic's; anywhere else (a data word, a law operand, a [..], arithmetic,
// an initializer) the address is used as a value and the edge is TAKEN.  A
// memory reference whose field is not one symbol gets an OPTEF_IMPLICIT edge
// to the decoded address, as do jda's deposit and entry words and cal's 100
// and 101.  Each word keeps its out and in lists and per-role in counts.

typedef enum
{
    OPTR_READ,      // the target is read as data: lac lio add sub and ior xor sad sas
                    // mul div idx isp
    OPTR_WRITE,     // the target is written: dac dap dip dio dzm idx isp, the word jda
                    // deposits into, location 100 for cal
    OPTR_JUMP,      // control transfers to the target: jmp jsp, the word after
                    // a jda entry, location 101 for cal
    OPTR_EXECUTE,   // the target is executed in place: xct
    OPTR_TAKEN,     // the target's address is used as a value
    OPTR_COUNT
} OptRole;

// Edge flags.
#define OPTEF_INDIRECT      0x01    // the instruction indirects: the edge goes to the
                                    // POINTER word; the eventual target is a second
                                    // edge (OPTEF_VIAPOINTER) or unknown
#define OPTEF_VIAPOINTER    0x02    // derived by following a pointer word that is
                                    // data, never written and holds a resolved address
#define OPTEF_PATCH         0x04    // dap or dip: only the address field is written
#define OPTEF_IMPLICIT      0x08    // no symbol names the target: jda's entry word and
                                    // the word after it, cal's 100 and 101, a memory
                                    // reference whose field is arithmetic or a number
#define OPTEF_CROSSBANK     0x10    // a via-pointer edge whose pointer names another
                                    // bank (a 16-bit pointer, valid only with eem on)
#define OPTEF_NOWORD        0x20    // nothing was emitted at the target address; toP
                                    // is NILP and the edge is only on the source's list
#define OPTEF_UNKNOWN       0x40    // an indirect edge whose pointer could not be
                                    // followed: the taken words of the bank were
                                    // marked conservatively instead
#define OPTEF_PLACEHOLDER   0x80    // the source word is patched and its field was
                                    // not a symbol ("rtn, jmp 0"): the edge records
                                    // the unpatched value only, is not followed, and
                                    // does not classify its target

// One reference edge.  Owned by the source word's out list; the target's in
// list only shares it.
typedef struct optedge
{
    struct optedge *nextOutP;   // next edge from the same source word
    struct optedge *nextInP;    // next edge into the same target word
    struct optword *fromP;      // the referencing word
    struct optword *toP;        // the referenced word, NILP with OPTEF_NOWORD
    int toBank;                 // the target address, kept even when no word is there
    int toAddr;
    OptRole role;
    unsigned int flags;         // OPTEF_ bits
    SymNodeP symP;              // the symbol the source wrote, NILP for an implicit edge
} OptEdge, *OptEdgeP;

// Basic blocks and reachability.
//
// The graph covers the words classified as code, less reserved table words
// and overlaid addresses, where which word the machine would fetch is
// unknowable.  OPTF_UNREACHED is laid over the code classification and never
// revises it.  Every edge that leaves the graph goes to one UNKNOWN sink (a
// NILP target block and an OptSink reason).  A sink edge carries no
// reachability: the analysis lost the thread, the thread did not end.

// Why an edge goes to the UNKNOWN sink instead of to a block.
typedef enum
{
    OPTSK_NONE,         // not a sink edge
    OPTSK_INDIRECT,     // the instruction indirects and the pointer could not be followed
    OPTSK_PATCHED,      // a dap or dip writes the address field, or a dac or dio the
                        //   whole word, and the code does not say what: the target varies
    OPTSK_NOWORD,       // nothing was emitted at the target address
    OPTSK_NOTCODE,      // a word is there, but it is not classified code
    OPTSK_OVERLAID,     // the target address holds more than one emitted word
    OPTSK_DEBREAK,      // "jmp i 1" in bank 0: the return from a sequence-break
                        // handler, whose target is whatever the break interrupted
    OPTSK_COUNT
} OptSink;

// What a control-flow edge represents.
typedef enum
{
    OPTFK_FALL,         // ordinary fall-through to the next address
    OPTFK_SKIP,         // a skip-class word skipping to .+2
    OPTFK_JUMP,         // jmp, to a decoded or followed target
    OPTFK_CALL,         // jsp, jda or cal entering a subroutine
    OPTFK_RETURN,       // the assumed return of a call, to the word after the call site
    OPTFK_RESUME,       // hlt continuing to .+1, which pressing Continue does
    OPTFK_COUNT
} OptFlowKind;

// An edge the instruction alone does not state, recovered from context and
// named so the report and -O=flow can say what was recovered.  Only an edge that reaches a block is marked; one that lands
// outside the graph is an ordinary sink with its own reason.
typedef enum
{
    OPTRV_NONE,         // the instruction states the edge itself
    OPTRV_IOT,          // the skip of an in-out transfer to a device that skips
    OPTRV_STORED,       // a target the code in front of a dac or dio of a whole
                        // jmp or jsp states
    OPTRV_COUNT
} OptRecovery;

// How a block ends.  The first four are the terminator instruction that ended
// it; the last three mean the block ran into something rather than ending of
// its own accord.
typedef enum
{
    OPTBE_SKIP,         // a skip-class word: skip group, isp, sad, sas, div
    OPTBE_JUMP,         // jmp
    OPTBE_CALL,         // jsp, jda, cal
    OPTBE_HALT,         // an operate word carrying the hlt bit
    OPTBE_BOUNDARY,     // the next word starts a block of its own
    OPTBE_NOTCODE,      // the next address holds a word the graph does not contain
    OPTBE_GAP,          // the next address holds no word, or is off the end of the bank
    OPTBE_COUNT
} OptBlockEnd;

// Why a word is an entry of the reachability walk.  A word can qualify under
// more than one; the statistics count it under the first that applies, in
// this order.
typedef enum
{
    OPTEN_START,        // the program's start address
    OPTEN_TAKEN,        // its address is used as a value, so an indirect jump or
                        // an xct could reach it
    OPTEN_EXPORT,       // it carries an exported label: another program may enter it
    OPTEN_SBS,          // it is a sequence-break handler entry: bank 0, address 4n+3
    OPTEN_COUNT
} OptEntryCause;

// Sequence-break frames.  Each channel owns four words of low core from address
// 0; the hardware stores AC, the packed PC and IO in the first three and
// executes the fourth, so channel n's handler entry is 4n+3.  The
// single-channel mode, the only one any shipped configuration enables, uses
// channel 0's frame: address 3.  optSbsChannels() reads the number of frames
// from the start address, and a program has none unless it holds an enable
// command (esm, asc or isb).
#define OPTSBS_ESM_DEVICE   055     // esm, 72xx55: the system on
#define OPTSBS_ASC_DEVICE   051     // asc, 72nn51: channel nn enabled
#define OPTSBS_ISB_DEVICE   052     // isb, 72nn52: a break simulated on channel nn
#define OPTSBS_CHANNELS    16      // channels in the full Type 20 system
#define OPTSBS_FRAMESIZE    4       // words of low core per channel
#define OPTSBS_ENTRYSLOT    3       // the word of a frame the hardware executes

// The address field of the DEBREAK instruction, "jmp i 1".  Recognized only
// in bank 0, which is where the hardware recognizes it.
#define OPTDEBREAK_ADDR     1

// Where a call actually returns to.
//
// A call's return edge normally goes to the word after it, but under the
// inline-argument convention the callee steps its saved return past argument
// words that follow the call:
//
//     jsp i [subr:0]
//     ARG:0                   // argument, read by the callee through its rtn
//     ...                     // the callee returns HERE
//
// optreturn.c classifies each call site.  An undecidable one keeps its edge at
// call + 1, marked assumed, never a hole: a hole would make the caller's tail
// look unreached and its callees uncalled, the unsound direction for T10.

// How sure the analysis is about where a call returns to.
typedef enum
{
    OPTRC_AFTER,        // proved: the callee returns to the word after the call
    OPTRC_STEPPED,      // proved: the callee steps its return word past N inline
                        // argument words, so it returns to call + 1 + N
    OPTRC_UNKNOWN,      // not decidable: the edge stays at call + 1, marked assumed
    OPTRC_COUNT
} OptReturnCase;

// Why a call site came out OPTRC_UNKNOWN; OPTRR_NONE for the proved cases.
// Each is a place the analysis stopped rather than guessed.
typedef enum
{
    OPTRR_NONE,         // the case is proved; no reason to give
    OPTRR_CALLEE,       // the call's own target is not one known word of the graph:
                        // a patched address field, or an indirection that could not
                        // follow, or two followed pointers that disagree
    OPTRR_NOSAVE,       // the callee's entry word does not save the return address
                        // with a direct, unpatched dac or dap, so there is no
                        // return word to follow
    OPTRR_CLOBBER,      // the return word is written by something this analysis
                        // did not account for, including any write through a
                        // pointer it could not follow
    OPTRR_DISAGREE,     // two paths arrive at the same word, or at the return,
                        // having stepped the return word different amounts
    OPTRR_NORETURN,     // no path was found that returns through the return word
    OPTRR_NESTED,       // a call inside the callee has no resolved return target,
                        // so where the path resumes after it is not known
    OPTRR_LOST,         // a path left the graph or ended somewhere unfollowable
    OPTRR_BUDGET,       // the walk hit its step limit before finishing
    OPTRR_COUNT
} OptReturnReason;

// One control-flow edge, owned by the successor list of the block it leaves.
// Predecessors are not listed; blocks only need to know how many arrive, and
// the dump prints successors.
typedef struct optflowedge
{
    struct optflowedge *nextP;  // next successor of the same block
    struct optblock *toP;       // the target block, NILP for the UNKNOWN sink
    int toBank;                 // where the edge points, meaningful only when toP is set
    int toAddr;
    OptFlowKind kind;
    OptSink sink;               // why it went to the sink, OPTSK_NONE when toP is set
    int crossbank;              // the edge leaves the bank of the word that made it
    int assumed;                // an OPTFK_RETURN edge left at call + 1 because the
                                // callee could not be read, not because it was proved
    OptRecovery recovered;      // OPTRV_NONE unless the edge was recovered
} OptFlowEdge, *OptFlowEdgeP;

// One basic block: a run of consecutive in-graph words at consecutive
// addresses in one bank, entered only at its first word.
typedef struct optblock
{
    struct optblock *nextP;     // next block, in bank then address order
    int id;                     // 1 based, assigned in that same order
    int bank;
    int startAddr;
    int endAddr;                // inclusive; equal to startAddr for a one-word block
    int wordCount;              // in-graph words in the block, always endAddr-startAddr+1
    struct optword *firstP;     // the word at startAddr
    struct optword *lastP;      // the word at endAddr, the one whose edges leave
    OptBlockEnd endKind;
    OptFlowEdgeP succP;         // successor edges, sink edges included
    OptFlowEdgeP succTailP;     // the last of them, for appending
    int succCount;
    int predCount;              // non-sink edges arriving from any block, this one included
    int isEntry;                // the first word is in the entry set
    int reached;                // the reachability walk arrived here
    int resumed;                // not reached, but a resumption seed reached it: a jmp or
                                // jsp somewhere names its first word.  Kept apart from
                                // reached because isEntry means more than "the walk starts
                                // here" to P1 and S4, and reached is what refuses a finding
    int resumeSeed;             // the block is one of those seeds
} OptBlock, *OptBlockP;

// One emitted word.  Entries are created in emission order, the order the
// code generators walk the tree, and never reordered.
typedef struct optword
{
    int bank;               // memory bank, 0 to MAXBANK
    int addr;               // address within the bank, 0 to BANKSIZE - 1
    int value;              // the 18-bit word, masked to WRDMASK; 0 when OPTF_RESERVED
    PNodeP nodeP;           // the statement node that emitted the word, NILP for a
                            // word from an automatically placed pool
    PNodeP exprP;           // the expression tree the value came from: the statement's
                            // rightP for OPTK_EXPR, the initializer for OPTK_TABLE and
                            // OPTK_VAR, the pooled expression for OPTK_CONST, NILP otherwise
    SymNodeP symP;          // the originating symbol for OPTK_VAR and OPTK_CONST, else NILP
    OptKind kind;           // what produced the word
    OptLabelP labelsP;      // every label defined at this bank and address
    const char *fileP;      // source file: the command line's until a FILENAME
                            // statement (a cpp line marker) names another
    int lineNo;             // source line the word came from, -1 if not known
    int index;              // position in emission order, 0 based
    unsigned int flags;     // OPTF_ bits
    struct optword *sameAddrP;  // next entry emitted at the same bank and address, NILP if none
    OptDecode decode;       // the decoded view and the spelling

    // References.  Out edges in discovery order (the expression's walk, then
    // implicit edges, via-pointer edges last); in edges in emission order.
    OptEdgeP outP;          // edges leaving this word
    OptEdgeP outTailP;      // the last of them, for appending
    OptEdgeP inP;           // edges entering this word: the reverse index
    OptEdgeP inTailP;
    int outCount;           // edges leaving, OPTEF_NOWORD ones included
    int inCounts[OPTR_COUNT];   // edges entering, per EFFECTIVE role: an indirect
                                // edge counts as a read of the pointer word it lands
                                // on, and a placeholder edge does not count at all
    unsigned int conflicts; // OPTCF_ bits

    // The word's block, NILP when it is not in the graph; asked this way so no
    // separate flag can disagree with it.
    OptBlockP blockP;

    // Return classification, meaningful only when isCallSite is set, which
    // optResolveReturns() does for every word that ends its block with a call.
    int isCallSite;                 // this word is a jsp, jda or cal
    int returnAddr;                 // where the return edge goes, in THIS word's bank
    int returnSteps;                // inline argument words the callee steps past,
                                    // 0 for the common case and for OPTRC_UNKNOWN
    int calleeBank;                 // the callee's entry word, -1 for either coordinate
    int calleeAddr;                 // when the call's target is not one known word
    OptReturnCase returnCase;
    OptReturnReason returnReason;
} OptWord, *OptWordP;

// Routines and the call graph.
//
// am1 has no procedures, so a routine is structural:
//
//   ENTRY   a block whose first word is the target of a call edge: jsp, jsp i,
//           jda or cal.
//   BODY    the blocks reachable from the entry without passing another entry.
//           Call edges are not followed; return edges are, since they continue
//           in the caller.
//   RETURN  the words that leave through jmp i rtn (jsp) or jmp i subr (jda).
//
// Three awkward shapes are counted, never silently decided:
//
//   1. Fall-in: a plain flow edge enters an entry.  The routine above is
//      truncated there and both ends are flagged (FALLSOUT, FALLIN).  Control
//      that arrives that way runs the second body with the first's return word
//      still live.
//   2. Two entries sharing a tail: the shared blocks belong to both routines
//      (OPTRT_SHARED), which are not merged, so every call the tail makes is
//      an arc from both.
//   3. A call the graph could not follow makes its routine a universal vertex
//      (OPTRT_CALLSUNKNOWN): it might call anything.  A body only unresolved
//      calls reach cannot be found; the opaque-entry count counts the
//      address-taken blocks outside every routine, where one would be.
//
// Recursion is a finding: the dap rtn idiom cannot survive a second call.
// Cycles are found on RESOLVED arcs only, so a reported cycle is real; those
// that exist only through a universal vertex are counted apart as artifacts.

// Which call idiom a routine's resolved callers use.  A jda routine's return
// address lives in its own entry word, fixed by where the routine sits, so jda
// routines are never T10 candidates.
typedef enum
{
    OPTRF_JSP,          // every resolved caller uses jsp or jsp i
    OPTRF_JDA,          // every resolved caller uses jda or cal
    OPTRF_MIXED,        // both
    OPTRF_COUNT
} OptRoutineForm;

// How the routine's return word was identified; independent of the return
// classification, which asks how far the return is stepped.
typedef enum
{
    OPTRW_NONE,         // no return word could be named
    OPTRW_SAVED,        // the entry word saves the return with a direct, unpatched
                        // dac or dap (the jsp idiom; Am1Includes' jda callees too)
    OPTRW_JDA,          // no save at the entry, but the callers are jda or cal, so
                        // the hardware has put the return address in the word one
                        // below the entry and the return is jmp i that word
    OPTRW_COUNT
} OptReturnWord;

// Per-routine flags.  Every one of them is a counted category, not a
// disqualification: a routine is still a routine with any combination set.
#define OPTRT_FALLIN        0x0001  // a plain flow edge from outside enters the entry
#define OPTRT_FALLSOUT      0x0002  // the body flows into another routine's entry and
                                    // stops there
#define OPTRT_SHARED        0x0004  // at least one body block also belongs to
                                    // another routine: two entries, one tail
#define OPTRT_CALLSUNKNOWN  0x0008  // a call in the body went to the sink: a universal vertex
#define OPTRT_JUMPSUNKNOWN  0x0010  // an indirect jump in the body was not followed.  Usually
                                    // its own jmp i rtn, so not a universal vertex; but a
                                    // pointer-table dispatch looks the same and leaves the
                                    // body incomplete
#define OPTRT_NORETURN      0x0020  // no return word was identified
#define OPTRT_OPENBODY      0x0200  // a body block is reachable without entering the
                                    // routine (an arm that jumps out, a shared tail, an
                                    // address-taken block), so it may run outside a call
#define OPTRT_ONCYCLE       0x0040  // on a cycle of RESOLVED call arcs: real
                                    // recursion or an escape, per the flag below
#define OPTRT_ESCCYCLE      0x0400  // the cycle is an escape, not recursion: an arc on it is
                                    // made from a block reachable without entering its
                                    // caller.  Decided once, for the dump and the report
#define OPTRT_ARTCYCLE      0x0080  // on a cycle only because a universal vertex was
                                    // introduced: an artifact, and labeled one
#define OPTRT_SBSPOOL       0x0100  // reachable from a sequence-break handler: it runs on
                                    // the second stack and interferes with the first

// One arc of the call graph: caller routine to callee routine.  Owned by the
// caller's out list.  Several call sites collapse into one arc and the count
// is kept, since "how many places call this" is a different question from
// "can these two be live at once".
typedef struct optcallarc
{
    struct optcallarc *nextP;   // next arc out of the same routine
    struct optroutine *toP;     // the callee
    int sites;                  // call sites in the caller that make this arc
    int openSites;              // of those, sites in a block reachable without entering
                                // the caller: what tells an escape from recursion
} OptCallArc, *OptCallArcP;

// One routine.  Built after the control-flow graph is complete and never
// mutates it: a routine points at blocks and words, and owns only its own arc
// list.
typedef struct optroutine
{
    struct optroutine *nextP;   // next routine, in bank then entry-address order
    int id;                     // 1 based, assigned in that same order
    int bank;                   // the entry word's bank
    int entryAddr;              // the entry word's address
    OptBlockP entryBlockP;      // the block that word begins
    OptRoutineForm form;
    int jspSites;               // resolved call sites reaching it with jsp
    int jdaSites;               // ... with jda or cal
    int mainLineSites;          // of all of them, the ones made from no routine
    int blockCount;             // blocks in the body
    int wordCount;              // words in them
    int *bodyIdsP;              // their ids, ascending, blockCount of them
    int returnBank;             // the word the return address is kept in, -1 for
    int returnAddr;             // either coordinate when there is none
    OptReturnWord returnWord;   // how that word was identified
    int returnCount;            // body words that leave through it
    int firstReturnAddr;        // the lowest such word, -1 if there are none
    unsigned int flags;         // OPTRT_ bits
    int scc;                    // 1-based strongly connected component, 0 before
                                // the graph is built
    OptCallArcP callsP;         // arcs out of this routine
    OptCallArcP callsTailP;     // the last of them, for appending
    int callArcs;               // how many
    int callerArcs;             // arcs INTO this routine, counted not listed
    int callDepth;              // the longest chain of routines this one heads, itself
                                // included (1 for a leaf); shared by a cyclic component,
                                // whose members may all be live at once
} OptRoutine, *OptRoutineP;

// The return-word sharing proposal (T10).
//
// ADVISORY ONLY, permanently: a wrongly shared return word fails as a wild jump
// in the one call ordering that overlaps, with nothing to catch it.  None of
// this is an OptFinding, none is counted in findingCounts[], and no
// optimization level reads it.

// One proposed sharing group: a set of return words that could be one word,
// and the routines that would share it.  Groups are built in bank, then pool,
// then color order and are never re-sorted, so two runs of the same source
// give the same report.
typedef struct optsharegroup
{
    struct optsharegroup *nextP;    // next group, in that order
    int bank;                   // the bank the return WORDS are in, not the entries
    int pool;                   // 0 main line, 1 sequence-break pool; colored apart, since
                                // a handler runs between any two words it interrupts
    int color;                  // the chain depth every member of this group carries
    int keepAddr;              // the surviving address: the group's lowest, for stability
    int wordCount;              // distinct return words in the group
    int freedWords;             // wordCount - 1, and 0 for a split-out word
    int unknownWords;           // of those words, ones held by a routine the graph does
                                // not fully know (open, jumpsunknown, callsunknown)
    int leafWords;              // of those words, ones every namer of which is a leaf
    int cyclicWords;            // of those words, ones a namer of which is on a cycle
    int splitOut;               // ONE word sharing nothing: two entries into one body
                                // name it at different call depths
    int checkFailed;            // the pairwise self-check found two members that can be
                                // on the call stack together; the report withholds it
    int memberCount;            // routines that would share the surviving word
    OptRoutineP *membersPP;     // them, in routine-id order
} OptShareGroup, *OptShareGroupP;

// Preconditions, rules and findings.
//
// A rule scans one bank in address order and, where its pattern matches,
// produces a finding: what it found, the suggested replacement and the saving.
// A match must pass P1 (no label or side entry on an interior word), P2 (no
// word written or taken), P3 (no xct target), P4 (no skip just before) and the
// rule's own conditions; one that fails becomes a suppressed finding carrying
// the reason.  P5 (no word shared with a device or a handler) cannot be
// established from the source and is stated in the report's header.

// The rules of the transform catalog, in the order the report groups them.
typedef enum
{
    OPTRULE_T1,         // two consecutive operate words merge into one
    OPTRULE_T1B,        // two consecutive shifts of the same opcode merge
    OPTRULE_T2,         // skip; jmp .+2; W becomes skip i; W
    OPTRULE_T3,         // jmp A where A holds jmp B becomes jmp B
    OPTRULE_T6,         // a cla before an instruction that loads AC outright
    OPTRULE_T7,         // a cli before a lio
    OPTRULE_T8A,        // lio [0] becomes cli
    OPTRULE_T8B,        // lac [0] becomes cla
    OPTRULE_T8C,        // lac [777777] becomes cla cma
    OPTRULE_T8D,        // lac [n] becomes law n, lac [~n] becomes law i n
    OPTRULE_T13,        // jmp .+1 is a no-operation
    OPTRULE_T14,        // a copy between AC and IO through a temporary
    OPTRULE_COUNT
} OptRuleId;

// Why a matched pattern was refused: the shared preconditions, then reasons
// belonging to one rule each.
typedef enum
{
    OPTWHY_NONE,        // nothing refused it: this is a live finding
    OPTWHY_P1_LABEL,    // P1: an interior word carries a label, so something
                        // may enter the pattern in the middle
    OPTWHY_P1_ENTRY,    // P1: an interior word starts a basic block, so control
                        // arrives there by an edge that carries no label
    OPTWHY_P2_WRITTEN,  // P2: a word of the pattern is written at run time
    OPTWHY_P2_TAKEN,    // P2: a word's address is used as a value
    OPTWHY_P3_XCT,      // P3: a word of the pattern is executed by an xct
    OPTWHY_P4_SKIP,     // P4: a skip-class word sits immediately before the pattern
    OPTWHY_PHASE,       // T1: source order disagrees with the hardware phase order
    OPTWHY_FLAGS,       // T1: both words operate on a program flag, differently
    OPTWHY_REPEAT,      // T1: a micro-op that is not idempotent is in both words
    OPTWHY_LAP,         // T1: the second word carries lap, whose value is its own
                        // address plus one, and merging moves it
    OPTWHY_EXCHANGE,    // T1: lia in one word and lai in the other are two copies;
                        // in a single word they are one exchange
    OPTWHY_SHIFTCOUNT,  // T1b: the summed shift count is more than nine
    OPTWHY_NOINVERSE,   // T2: the skip-class word has no inverted form (isp, div)
    OPTWHY_PATCHED,     // T3, T8: the word the rule reads through is patched
    OPTWHY_TEMPUSED,    // T14: the temporary is reached from outside the pattern
    OPTWHY_THROUGH,     // T3, T8: the word the rule reads through has its address
                        // taken, so a write through a pointer can change it
    OPTWHY_UNREACHED,   // a word of the pattern is in a block no entry reaches,
                        // in a run no label names, so nothing shows it is an
                        // instruction
    OPTWHY_TABLE,       // the pattern is in a block entered only through its
                        // taken address that runs into a word that is not code:
                        // a table an xct or a pointer uses
    OPTWHY_COUNT
} OptWhy;

// The longest pattern any rule matches: T14's four-word exchange.
#define OPTFIND_MAXWORDS    4

// Where a finding's pattern sits relative to the declared regions.  Never a
// refusal: the report advises everywhere, and a region is permission to
// REWRITE, not a condition on being told.
typedef enum
{
    OPTREG_NOREGIONS,   // the source declares no region: nothing is marked
    OPTREG_INSIDE,      // every word of the pattern is inside one region
    OPTREG_OUTSIDE,     // no word of it is inside any region
    OPTREG_PARTIAL,     // the pattern straddles a region boundary, so no transform
                        // can fire on it
    OPTREG_COUNT
} OptRegionPlace;

// The heuristics -O2 uses in place of P5 (optguess.c).
typedef enum
{
    OPTGH_H1,           // handler territory
    OPTGH_H2,           // delay loop
    OPTGH_H3,           // device loop
    OPTGH_H4,           // device buffer
    OPTGH_H5,           // absolute address
    OPTGH_H6,           // computed address
    OPTGH_COUNT
} OptGuessId;

// The heuristics -O2 applies, as bits (1 << OptGuessId).  Inside an optimize
// region only OPTGH_INREGION's apply: there H2 to H5 are skipped.
#define OPTGH_AUTHORIZED    ((1u << OPTGH_H1) | (1u << OPTGH_H2) | (1u << OPTGH_H3) | \
                             (1u << OPTGH_H4) | (1u << OPTGH_H5))
#define OPTGH_INREGION      (1u << OPTGH_H1)

// The heuristics that refuse a rewrite changing the program's length at every
// level, in a region or not, and never one that keeps it (T3, T8a-d).  H6's
// run is indexed by an address the program computes at run time, which no
// declaration can vouch for: a word taken out of it or put into it moves every
// entry after it.
#define OPTGH_LENGTH        (1u << OPTGH_H6)

// One finding, live or suppressed: the pattern's words in address order, and
// strings the finding owns.
typedef struct optfinding
{
    struct optfinding *nextP;
    OptRuleId rule;
    OptWhy why;             // OPTWHY_NONE when the finding is live
    OptRegionPlace place;   // where the pattern sits in the regions
    int bank;               // where the pattern starts
    int addr;
    int wordCount;          // words in the pattern
    OptWordP wordsP[OPTFIND_MAXWORDS];
    int saveWords;          // words of code the rewrite removes
    int saveTemps;          // storage words it additionally frees (T14)
    int saveTime;           // microseconds it removes from one execution
    int perHop;             // the saving is per executed hop, not once (T3)
    char *replaceP;         // the suggested source, in am1 syntax
    char *detailP;          // the rule's note, or the evidence for the refusal
    // Live findings only: the heuristics that would refuse it, as bits, and for
    // each the footprint word it marked.  Set by optBuildGuess().
    unsigned int guessBits;
    struct optword *guessWordP[OPTGH_COUNT];
} OptFinding, *OptFindingP;

// One optimize/endoptimize region: a span of EMISSION ORDER, not addresses, so
// a bank change, an overlay or a macro inside one needs no arithmetic.  Regions
// do not nest and the builder never reorders, so every entry from firstIndex to
// lastIndex (in tableP->entriesPP) is in this region alone.  An empty region is
// kept: it tells an author who deleted the code inside one.
typedef struct optregion
{
    struct optregion *nextP;    // next region, in source order
    int id;                     // 1 based, in that order
    int openLine;               // the line 'optimize' is on
    int endLine;                // the line 'endoptimize' is on
    const char *fileP;          // the file both are in; a region may not cross one
    int openBank;               // the bank and pc the region opened at, which are
    int openAddr;               // where the NEXT word would have gone
    int firstIndex;             // first and last entry in emission order, -1 both
    int lastIndex;              // when the region holds no emitted word
    int wordCount;              // entries between them, which is lastIndex-firstIndex+1
    int contradictions;         // of those, words marked written, taken or patched:
                                // the declaration and the evidence disagree
} OptRegion, *OptRegionP;

// One 'inline NAME' marking.  A POINT, not a span: it names a routine to inline
// at every call site, overriding the size cap and the bank budget.
// optCheckRegions() resolves the name once every bank is settled; a name that
// matches no routine is reported, not an error, since a renamed routine leaves
// its marking behind.
typedef struct optinlinemark
{
    struct optinlinemark *nextP;    // next marking, in source order
    int id;                         // 1 based, in that order
    const char *nameP;              // the routine's name, as the author spelled it
    const char *fileP;              // where the directive was written
    int line;                       // and the line it was on
    int declBank;                   // the bank and pc the directive stood at, which
    int declAddr;                   // are where the NEXT word would have gone
    OptRoutineP routineP;           // the routine it names, NILP when none matched
    int dupOf;                      // the id of an earlier marking naming the same
                                    // routine, 0 when this is the first
} OptInlineMark, *OptInlineMarkP;

// A '%%ceiling EXPR' directive: the first word of its bank a length-changing
// rewrite may not grow into.  It protects storage filled only at run time,
// which no assembled word reveals.  It only lowers the default (07750 in bank 0, 07777 elsewhere) and the lowest in
// a bank wins.  The parser refuses 0 and values above the default;
// optCheckRegions() refuses one the bank's assembled words already reach.
typedef struct optceiling
{
    struct optceiling *nextP;       // next directive, in source order
    int id;                         // 1 based, in that order
    int bank;                       // the bank it names: the one it was written in
    int value;                      // the first word no rewrite may reach, 1 to 07777
    const char *fileP;              // where the directive was written
    int line;                       // and the line it was on
} OptCeiling, *OptCeilingP;

// What -O1 or -O2 did with each live finding.  Every live finding gets exactly
// one fate, so the fates add up to findingCount.  A suppressed finding gets no
// record, and the finding list is the same with or without a level.
typedef enum
{
    OPTXF_FIRED,            // rewritten (or, in a dry run, would be)
    OPTXF_NOREGIONS,        // the source declares no region, so nothing may fire
    OPTXF_OUTSIDE,          // no word of the pattern is inside a region
    OPTXF_PARTIAL,          // the pattern straddles a region boundary
    OPTXF_NOTCARRIED,       // a rule the level does not carry: it changes the length
                            // of the program
    OPTXF_CYCLE,            // T3 only: the chain of jmps comes back to a word
                            // already on it, or to the rewritten word itself
    OPTXF_LONG,             // T3 only: the chain did not end within
                            // OPT_MAXCHAIN hops.  Refused, not cut short
    OPTXF_UNREPRESENTABLE,  // the word is not the whole expression of one
                            // statement, so there is no expression to replace
    // The fates below are kept last so a dump that cannot produce them keeps
    // its order; the dump prints them only when they can be non-zero.
    OPTXF_HANDSOFF,         // a word the rewrite involves is in a nooptimize span
    OPTXF_GUESSED,          // a heuristic that applies here marks a word the rewrite
                            // involves: -O2's, or H6 on a deletion at any level
    OPTXF_OFF,              // it would have fired, and -O=upto, -O=range or -O=off
                            // switched it off
    OPTXF_TIMED,            // H2 or H3 marks a word it involves: a delay or device
                            // loop, whose timing a deletion would change, at any level
    OPTXF_OVERLAP,          // a word it involves is a word another fired rewrite
                            // makes, copies, moves or deletes
    OPTXF_COUNT
} OptXformFate;

// One live finding's rewrite record.
typedef struct optxform
{
    struct optxform *nextP;
    OptFindingP findingP;   // the live finding, which owns its words
    OptXformFate fate;
    OptWordP throughP;      // the word the rule reads through -- T8's pool word, T3's
                            // intermediate jmp -- when the fate is FIRED, else NILP
    OptRegionP regionP;     // the region the rewritten word is in, when FIRED
    int before;             // the word's value as assembled from the source
    int after;              // and as rewritten; equal to before unless FIRED
    char *beforeP;          // both as source, owned here; NILP unless FIRED
    char *afterP;
    // -O2 only: for FIRED, the heuristics whose guess it rests on (every one
    // that applied and found nothing); for GUESSED, the ones that refused it.
    unsigned int assumed;
    // T3, FIRED: the jmps jumped past, in run order, owned here.  chainPP[0] is
    // throughP and chainPP[hops-1] holds the far target.  stopP is the word the
    // walk declined (a nooptimize span, or an -O2 heuristic), NILP when the
    // chain simply ended.
    OptWordP *chainPP;
    int hops;
    OptWordP stopP;
    // Set only under bisection: the record's place, from 1, among those that
    // would fire (0 for any other), and the OPTBIS_ bits that switched it off.
    int ordinal;
    unsigned int offBy;
    // T3, FIRED: numberP is the INTEGER operand of "jmp N" when the far target
    // had to be written as a number (NILP for a symbol), and targetP the word
    // there; relayout re-points numberP at targetP's new address.  T8d sets
    // numberP to its law operand, targetP NILP, and relayout recomputes it from
    // the replaced constant, which may hold an address that moved.  Both belong
    // to the tree and the table, not to the record.
    PNodeP numberP;
    OptWordP targetP;
    // Deleting rules (T1, T1b, T2, T6, T7, T13), FIRED: delP is the word
    // relayout deletes; keepP the word rewritten in its place (NILP for T6, T7,
    // T13) and treeP its new expression, installed and withdrawn together with
    // the delete so a program never holds one without the other.  outcomeP is
    // relayout's verdict, NILP until it runs.
    OptWordP delP;
    OptWordP keepP;
    PNodeP treeP;
    const char *outcomeP;
    // T8 only, set on the first fired T8 reading a pool word nothing else
    // names.  freesPool: the word is freed.  licensedPool: every fired T8
    // reading it is in a declared region; without that relayout keeps the word,
    // since reclaiming it would change the length with no declaration.
    int freesPool;
    int licensedPool;
    int poolCollected;      // and relayout took it: 0 until relayout has run
} OptXform, *OptXformP;

// The bisection modifiers, as bits, and how many there are.
#define OPTBIS_UPTO     1u          // -O=upto=N
#define OPTBIS_RANGE    2u          // -O=range=B:LO-HI
#define OPTBIS_OFF      4u          // -O=off=FILE
#define OPTBIS_COUNT    3

// S1, inline expansion.
//
// One call site as optInlineJudge() (optspeed.c) sees it, shared by -O=speed
// and the rewrite so the two cannot disagree.  The copy is the callee's body
// less its entry (the save word) and its last word, which must be a return,
// so the copy falls through to the caller's next word.  Other returns, and
// direct jmps to that last word, become "jmp .+k" to the word after the copy.
// A jsp is replaced by the copy; a jda stays as "dac Y" and the copy follows.
typedef struct optinlineshape
{
    int why;                    // the first reason that refuses it, optInlineWhyName() spells it
    int eligible;               // no reason before the guess refuses it
    unsigned int guessBits;     // every heuristic marking the site or a body word; recorded,
                                // not applied: -O=speed refuses on any, -O2 on those that
                                // apply where the site is, -O1 on none
    char detail[80];            // notcopyable: the word that broke the copy, " at 0740 names 0750"
    OptRoutineP routineP;       // the callee, once one was found
    OptWordP entryP;            // its entry, the save word, which is not copied
    OptWordP rtnP;              // its return word: the patched "rtn, jmp ." or the data word
    OptWordP lastP;             // the body's last word by address, the dropped return
    int isJda;                  // the site is a jda, which stays as "dac Y"
    int dapForm;                // the return word is the patched "jmp ." in the body
    int dropsRtn;               // the last word is a return and is not copied
    int body;                   // the callee's words
    int copy;                   // the words the copy holds: body less the save and the return
    int spent;                  // what the program grows by at the site: copy less the call
                                // word for a jsp, the copy itself for a jda
    int best;                   // microseconds saved per execution, by the best return
    int worst;                  // and by the worst
    int single;                 // the callee has this one site and no other way in
    int freedConsts;            // pool words only this site names, freed with the callee
    int net;                    // single: what the program grows by with the callee deleted
} OptInlineShape, *OptInlineShapeP;

// What the rewrite did with one call site inside a declaration.  Only a site a
// declaration reaches is recorded at all -- its word in a speed region, or its
// callee marked 'inline' -- so a source that declares neither has no record.
// The order is the order the questions are asked in.
typedef enum
{
    OPTIN_FIRED,            // copied (the copy is relayout's edit; its outcome is recorded
                            // apart, below, since relayout runs after the fates)
    OPTIN_REFUSED,          // the judge refused it: shape.why says for what
    OPTIN_HANDSOFF,         // the site or a body word is in a nooptimize span
    OPTIN_GUESSED,          // a heuristic that applies here marks the site or the body: -O2's,
                            // or H6 at any level
    OPTIN_UNREPRESENTABLE,  // the site, or a word the copy retargets, is not one statement's
                            // whole expression, or a jda's operand is not one name
    OPTIN_NESTED,           // the site lies in a body another fired inline copies, or its
                            // body holds a fired site: one copy may not contain another
    OPTIN_CAP,              // a body over the cap, neither single-site nor marked
    OPTIN_BUDGET,           // the bank's budget was spent on cheaper sites
    OPTIN_FULL,             // a marked or single-site copy would pass the bank's ceiling
    OPTIN_OFF,              // it would have fired, and a bisection modifier switched it off
    OPTIN_COUNT
} OptInlineFate;

typedef struct optinline
{
    struct optinline *nextP;
    OptWordP siteP;             // the call word
    OptInlineShape shape;       // the judge's verdict on it
    OptInlineFate fate;
    int marked;                 // its callee carries an 'inline' marking
    int inSpeed;                // its word is in a speed region
    int deletes;                // FIRED single-site: the callee is deleted once the copy stands
    unsigned int assumed;       // -O2: the heuristics it rests on (FIRED) or that refused it
                                // (GUESSED), as OptXform's; 0 under -O1
    int ordinal;                // its place among the rewrites that would fire, from
    unsigned int offBy;         // 1, after every T3 and T8; and the modifiers that switched it off
    // The edit, built by opttransform.c for a FIRED site and made by relayout.
    PNodeP prefixP;             // jda: the "dac Y" tree; NILP for a jsp
    int retargetCount;          // words of the body that become "jmp .+k" in the copy
    OptWordP *retargetsPP;      // those words, owned here
    PNodeP *retargetTreesPP;    // and their trees, owned by the tree once installed
    PNodeP *offsetsPP;          // each tree's k, which relayout sets from the layout it sees
    // What relayout made of it.  outcome is -1 until relayout runs.
    int outcome;                // relayout's reason for the copy: 0 is accepted
    const char *outcomeP;       // its name
    int deleted;                // words of the callee relayout deleted
    int deleteRefused;          // and the deletes it refused or skipped
} OptInline, *OptInlineP;

// S4, fall-through placement.  A site is a direct jmp J, last in its reached
// block, whose target T starts a block with J's block as its only predecessor.
// The run is T's block and those following it by address until one ends in a
// jmp.  Moving the run after J and deleting J saves a word and a jump on every
// execution.  optPlaceJudge() (optspeed.c) decides it for -O=speed and the
// rewrite alike.
typedef struct optplaceshape
{
    int why;                    // the first reason that refuses it, optPlaceWhyName() spells it
    int eligible;               // no reason before the guess refuses it
    unsigned int guessBits;     // every heuristic marking J or a run word; recorded, not applied
    OptWordP jmpP;              // J, the word deleted
    OptWordP targetP;           // T, the run's first word
    OptWordP endP;              // the run's last word, a jmp; NILP until the run is found
    int run;                    // the run's words
    int save;                   // microseconds saved per execution: J's time
} OptPlaceShape, *OptPlaceShapeP;

// What the rewrite did with one placement site.  Only a site whose J is in an
// optimize or speed region is recorded -- or, under -O2 with -O=undeclared,
// every site -- so a source that declares nothing, built without the switch,
// has no record.
typedef enum
{
    OPTPL_FIRED,            // moved and J deleted (relayout's edits; their outcome is recorded
                            // apart, below)
    OPTPL_REFUSED,          // the judge refused it: shape.why says for what
    OPTPL_PARTIAL,          // J is declared and a run word is not, and the switch is not given
    OPTPL_HANDSOFF,         // J or a run word is in a nooptimize span
    OPTPL_GUESSED,          // a heuristic that applies marks J or a run word: -O2's, or H6
                            // at any level
    OPTPL_OVERLAP,          // J or the run touches a site taken before it, a fired inline's
                            // call or callee, or a word a fired T3 or T8 rewrites
    OPTPL_OFF,              // it would have fired, and a bisection modifier switched it off
    OPTPL_COUNT
} OptPlaceFate;

typedef struct optplace
{
    struct optplace *nextP;
    OptPlaceShape shape;        // the judge's verdict on it
    OptPlaceFate fate;
    int declared;               // J is in an optimize or speed region; 0: the switch reached it
    unsigned int assumed;       // -O2: the heuristics it rests on (FIRED) or that refused it
                                // (GUESSED); 0 under -O1
    int ordinal;                // its place among the rewrites that would fire, after
    unsigned int offBy;         // every T3, T8 and inline; and the modifiers that switched it off
    // What relayout made of it.  outcome is -1 until relayout runs.
    int outcome;                // relayout's reason for the move: 0 is accepted
    const char *outcomeP;       // its name
    int deleteOutcome;          // and for J's delete, -1 when it was not tried
    const char *deleteOutcomeP;
} OptPlace, *OptPlaceP;

// S2, loop unrolling.  A site is a counted loop:
//
//     S   law i n          (or lac K, K a constant word holding -n)
//     D   dac c
//     H   body, B words    H labeled on its own line or on the word's
//     I   isp c
//     J   jmp H
//
// It runs n trips (isp on -1 leaves +0 and skips).
// The rewrite writes the body n times and deletes S, D, I and J; the dead
// counter stays, since it may be a variable relayout cannot delete.  Only a
// straight-line body is copied (no label but H's, no word naming a loop word),
// so no copy needs re-pointing.  optUnrollJudge() (optspeed.c) decides it for
// -O=speed and the rewrite alike.
typedef struct optunrollshape
{
    int why;                    // the first reason that refuses it, optUnrollWhyName() spells it
    int eligible;               // no reason before the guess refuses it
    unsigned int guessBits;     // every heuristic marking a word from S to J
    char detail[80];            // internal and used: the word that refused it, " at 0403 label x"
    OptWordP setP;              // S, law i n or lac K; NILP when there is none
    OptWordP depP;              // D, dac c
    OptWordP headP;             // H, the body's first word
    OptWordP ispP;              // I
    OptWordP jmpP;              // J
    OptWordP ctrP;              // the counter c, NILP when it is not a word of the program
    int trips;                  // n, 0 when the count is not a constant
    int body;                   // B, the body's words
    int unrolled;               // n * B, the words the body becomes
    int spent;                  // what the program grows by: (n - 1) * B less S, D, I and J
    int save;                   // microseconds per pass through the loop
} OptUnrollShape, *OptUnrollShapeP;

// What the rewrite did with one loop.  Only a loop whose isp is in an optimize
// or a speed region is recorded, so a source that declares neither has no
// record.  The order is the order the questions are asked in.
typedef enum
{
    OPTUN_FIRED,            // unrolled (relayout's edits; their outcome is recorded apart)
    OPTUN_REFUSED,          // the judge refused it: shape.why says for what
    OPTUN_PARTIAL,          // the isp is declared and a word from S to J is not
    OPTUN_HANDSOFF,         // a word from S to J is in a nooptimize span
    OPTUN_GUESSED,          // a heuristic marks a word from S to J, at every level
    OPTUN_OVERLAP,          // a word from S to J is touched by a fired inline, placement or
                            // earlier unroll, or rewritten by a fired T3 or T8
    OPTUN_CAP,              // n * B over the cap
    OPTUN_BUDGET,           // the bank's free words, less the inlines' and the reserve, are too few
    OPTUN_OFF,              // it would have fired, and a bisection modifier switched it off
    OPTUN_COUNT
} OptUnrollFate;

typedef struct optunroll
{
    struct optunroll *nextP;
    OptUnrollShape shape;       // the judge's verdict on it
    OptUnrollFate fate;
    unsigned int assumed;       // the heuristics that refused it (GUESSED); 0 otherwise
    int ordinal;                // its place among the rewrites that would fire, after
    unsigned int offBy;         // every T3, T8, inline and placement; and the modifiers that
                                // switched it off
    // What relayout made of it.  outcome is -1 until relayout runs.
    int outcome;                // relayout's reason for the unroll: 0 is accepted
    const char *outcomeP;       // its name
    int deleted;                // setup words (S and D) relayout deleted
    int deleteRefused;          // and the ones it refused or skipped
} OptUnroll, *OptUnrollP;

// Deleting an eem or lem the extend-window analysis (optwindow.c) shows does
// nothing.  Only a word in an optimize or speed region is recorded.
// Any subset of the redundant words and dead lems may go together; a freed eem
// is redundant only once the redundant and dead lems it depends on are gone, so
// it goes only with every one of them.
typedef enum
{
    OPTWD_REDUNDANT_EEM,    // the window is already open
    OPTWD_REDUNDANT_LEM,    // the window is already closed
    OPTWD_DEAD_LEM,         // nothing depends on the window before it is set again
    OPTWD_FREED_EEM,        // redundant once the lems in depsPP are deleted
    OPTWD_KIND_COUNT
} OptWinDelKind;

// The order is the order the questions are asked in.
typedef enum
{
    OPTWD_FIRED,            // deleted (relayout's edit; its outcome is recorded apart)
    OPTWD_HANDSOFF,         // the word is in a nooptimize span
    OPTWD_GUESSED,          // a heuristic that applies here marks the word: -O2's, or H6 at
                            // any level
    OPTWD_USED,             // the word is read as data, so deleting it changes a value
    OPTWD_UNREPRESENTABLE,  // not one statement's whole, single word
    OPTWD_OVERLAP,          // another fired rewrite makes, copies, moves or deletes the
                            // word, or deletes the word before it
    OPTWD_DEPENDS,          // a freed eem one of whose lems is not deleted
    OPTWD_OFF,              // it would have fired, and a bisection modifier switched it
                            // (or, for a freed eem, one of its lems) off
    OPTWD_COUNT
} OptWinDelFate;

typedef struct optwindel
{
    struct optwindel *nextP;
    OptWordP wordP;
    OptWinDelKind kind;
    OptWinDelFate fate;
    unsigned int assumed;       // -O2: the heuristics that refused it (GUESSED); 0 otherwise
    OptWordP *depsPP;           // a freed eem: the lems it depends on, owned here
    int depCount;
    int ordinal;                // its place among the rewrites that would fire, after
    unsigned int offBy;         // every deletion; and the modifiers that switched it off
    // What relayout made of it.  outcome is -1 until relayout runs.
    int outcome;                // relayout's reason: 0 is accepted
    const char *outcomeP;       // its name
} OptWinDel, *OptWinDelP;

// The per-bank index: one pointer per address, to the FIRST entry emitted
// there.  Later entries at the same address (overlays) chain through
// sameAddrP.  Only banks that emitted at least one word have an index.
typedef struct optbank
{
    int bank;               // the bank number this index covers
    int count;              // entries in this bank, overlays included
    OptWordP wordsP[BANKSIZE];  // first entry at each address, NILP if none
} OptBank, *OptBankP;

// A label the builder saw, before it is attached to entries.  Kept on the
// table so a label whose address has no entry (a label at the very end of a
// program, or one on a reserved table word) is still on record.
typedef struct optlabeldef
{
    struct optlabeldef *nextP;
    SymNodeP symP;          // the label's symbol
    int bank;               // where it was defined
    int addr;
    int attached;           // non-zero once it has been attached to at least one entry
} OptLabelDef, *OptLabelDefP;

// The word table itself.
typedef struct opttable
{
    OptWordP *entriesPP;    // every entry, in emission order
    int count;              // entries used
    int capacity;           // entries allocated
    OptBankP banksP[MAXBANK + 1];   // index per bank, NILP for a bank that emitted nothing
    OptLabelDefP labelsP;   // every label definition seen, most recent first
    int labelCount;         // how many
    int reservedCount;      // entries flagged OPTF_RESERVED
    int dupCount;           // entries flagged OPTF_DUPADDR
    int mismatchCount;      // entries flagged OPTF_PCMISMATCH
    int hasStart;           // non-zero if the program ended with 'start', zero for 'stop'
    int startAddr;          // the start address when hasStart is set

    // The sequence-break enable commands, counted on first use by
    // optSbsEnableCount().
    int sbsEnablesCounted;  // non-zero once the two below are filled
    int sbsEnableCount;     // words that are esm, asc or isb, in any form
    struct optword *sbsFirstEnableP;    // the first of them in emission order, NILP if none

    // Reference statistics, filled by optBuildReferences().
    int edgeCount;              // every edge made, OPTEF_NOWORD ones included
    int roleCounts[OPTR_COUNT]; // of those, per role
    int indirectCount;          // edges with OPTEF_INDIRECT
    int viaCount;               // second edges made by following a constant pointer
    int unknownIndirectCount;   // indirect edges whose pointer could not be followed
    int placeholderCount;       // edges flagged OPTEF_PLACEHOLDER
    int unresolvedCount;        // symbol references that never resolved: logged and skipped
    int noWordCount;            // edges to an address at which nothing was emitted
    int conservativeBanks[MAXBANK + 1];    // per bank, the OPTF_MAYBE_ marks applied
                                            // there as a bit set, 0 if none

    // The control-flow overlay, filled by optBuildFlow().  Per-bank figures are
    // recomputed from the block list when printed.
    OptBlockP blocksP;              // every block, in bank then address order
    OptBlockP blocksTailP;          // the last of them, for appending
    int blockCount;
    int graphWords;                 // words the graph holds, the sum of wordCount
    int notCodeWords;               // words left out as not classified code
    int overlaidWords;              // words left out because their address is overlaid
    int largestBlock;               // words in the largest block, 0 if there are none
    int flowEdgeCount;              // every edge made, sink edges included
    int flowKindCounts[OPTFK_COUNT];    // of those, per kind
    int sinkEdgeCount;              // how many went to the UNKNOWN sink
    int sinkCounts[OPTSK_COUNT];    // of those, per reason
    int entryCount;                 // words in the entry set
    int entryCounts[OPTEN_COUNT];   // of those, per cause, first cause wins
    int reachedBlocks;
    int unreachedBlocks;
    int unreachedWords;             // words flagged OPTF_UNREACHED
    int resumeSeeds;                // blocks a jmp or jsp names that the walk never reached
    int resumedBlocks;              // of unreachedBlocks, those a resumption reaches
    int resumedWords;               // of unreachedWords, those in a resumed block
    int xctOnlyWords;               // words flagged OPTF_XCTONLY

    // Return classification, filled by optResolveReturns() before the blocks
    // are formed, since a stepped return's target must begin a block.
    int returnSiteCount;                    // words that end their block with a call
    int returnCaseCounts[OPTRC_COUNT];      // of those, per case
    int returnReasonCounts[OPTRR_COUNT];    // of the unknown ones, per reason
    int returnJdaSites;                     // of the sites, how many are jda or cal
    int returnMovedSites;                   // sites whose return edge moved off call+1
    int returnCalleeCount;                  // distinct callee entries analyzed

    // The routines (optBuildRoutines()) and the call graph over them
    // (optBuildCallGraph()), in bank then address order, never re-sorted.
    OptRoutineP routinesP;              // every routine, in that order
    OptRoutineP routinesTailP;          // the last of them, for appending
    int routineCount;
    int routineFormCounts[OPTRF_COUNT]; // of those, per call idiom
    int routineWordKinds[OPTRW_COUNT];  // of those, per return-word derivation
    int routineBlocks;                  // blocks belonging to at least one routine
    int routineWords;                   // words in them
    int outsideBlocks;                  // blocks belonging to none: the main line,
    int outsideWords;                   // plus anything nothing reaches
    int callSiteCount;                  // blocks ending in a call with a resolved target
    int callSiteSinks;                  // blocks ending in a call that went to the sink
    int mainLineSites;                  // resolved call sites made from no routine
    int fallInRoutines;                 // category 1: entries a plain edge also enters
    int fallInEdges;                    // the edges that do it
    int sharedBlocks;                   // category 2: blocks owned by more than one
    int sharedRoutines;                 // routines owning at least one of them
    int opaqueEntries;                  // category 3: address-taken blocks in no routine
    int unknownCallRoutines;            // routines flagged OPTRT_CALLSUNKNOWN
    int unknownJumpRoutines;            // routines flagged OPTRT_JUMPSUNKNOWN
    int noReturnRoutines;               // routines with no return word identified
    int openRoutines;                   // routines flagged OPTRT_OPENBODY
    unsigned char *blockOwnersP;        // routines owning each block, by 1-based block
    int blockOwnerSlots;                // id; saturates at 255, since only 0, 1 or more matters

    int callArcCount;                   // distinct routine-to-routine arcs
    int callArcSites;                   // call sites those arcs stand for
    int sccCount;                       // strongly connected components of the arcs
    int cycleCount;                     // of those, components that are a cycle
    int cycleRoutines;                  // routines on one, of either class below
    int defectCycles;                   // cycles no escaping routine is on: real
    int defectCycleRoutines;            // defects, the only class the report names
    int escapeCycles;                   // cycles that hold at least one OPTRT_OPENBODY
    int escapeCycleRoutines;            // routine, so the arc back is a transfer out
                                        // of a routine and not a second call
    int artifactCycleRoutines;          // routines on a cycle only through the
                                        // universal vertex: an artifact, labeled one
    int closurePairs;                   // ordered pairs in the transitive closure
    unsigned int *callReachPP;          // the closure: routineCount bit vectors, each
    int callReachWords;                 // callReachWords unsigned ints long
    int sbsHandlers;                    // sequence-break handler entry words in the graph
    int sbsPoolRoutines;                // routines reachable from one

    // The call-depth measurement.  The return words saved are the candidates
    // minus the longest call chain (Mirsky's theorem).  Only the per-bank
    // figures are real: a return word shares only within its own bank, so
    // callDepthSaving is an upper bound no layout can collect.
    int returnWordCount;                // distinct return words, the candidate set
    int sharedReturnWords;              // return words more than one routine claims,
                                        // counted so the two totals reconcile
    int callMaxDepth;                   // the longest chain of routines holding a
                                        // return word, over the whole program
    int callDepthSaving;                // returnWordCount - callMaxDepth, ignoring
                                        // banks.  An upper bound, not a proposal
    int bankReturnWords[MAXBANK + 1];   // per bank, by the bank the WORD is in and
    int bankMaxDepth[MAXBANK + 1];      // not the bank its routine's entry is in.
                                        // The depth is over the MAIN LINE alone
    int bankSaving[MAXBANK + 1];        // the two pools' savings, added
    int bankLeafWords[MAXBANK + 1];     // of those words, the ones held by a leaf
    int bankPoolWords[MAXBANK + 1];     // of those words, the ones held by a routine a
    int bankPoolDepth[MAXBANK + 1];     // handler can reach, and the longest chain among
                                        // them: the second pool, colored apart, since such
                                        // a routine can run between any two main-line words
    int bankPoolLeafWords[MAXBANK + 1]; // leaf words inside that second pool
    int bankSavingTotal;                // the per-bank savings summed: the figure a
                                        // layout could really collect
    int leafRoutines;                   // routines that make no call and leave no jump
                                        // unresolved: the sound cut, needing no call graph
    int leafSaving;                     // what that cut alone recovers, per bank,
                                        // summed.  One word survives in each bank

    // The sharing proposal, filled by optBuildSharing().  ADVISORY ONLY.
    OptShareGroupP shareGroupsP;        // every proposed group, in bank then pool
    OptShareGroupP shareGroupsTailP;    // then color order, never re-sorted
    int shareGroupCount;
    int shareFreedWords;                // words the whole assignment would free; normally
                                        // bankSavingTotal, the report explains a difference
    int shareBankFreed[MAXBANK + 1];    // per bank, against bankSaving[] above
    int shareBankJda[MAXBANK + 1];      // per bank, the words the jda exclusion took
    int shareLeafFreed;                 // what the leaf cut alone would free, over the
                                        // candidate set.  Against leafSaving above
    int shareUnknownWords;              // words proposed on a routine the graph does not
                                        // fully know: where a reviewer should look hardest
    int shareSplitWords;                // words two entries name at different call
                                        // depths.  Excluded from sharing entirely
    int shareJdaWords;                  // words excluded because the jda form fixes the
                                        // word's address at the routine's own
    int shareCheckFailures;             // groups the pairwise self-check refused.  Any
                                        // at all is a defect in this analysis
    int shareDisagreeBanks;             // banks where the assignment and the measurement differ

    // The optimize/endoptimize regions: recorded by the table builder, checked
    // by optCheckRegions() once the reference evidence exists.  None of this is
    // a finding; regionFindings[] classifies the live list.
    OptRegionP regionsP;                // every region declared, in source order
    OptRegionP regionsTailP;            // the last of them, for appending
    int regionCount;                    // how many; zero is the ordinary case
    int regionWords;                    // emitted words inside any region
    int regionBankWords[MAXBANK + 1];   // of those, per bank: a region may span a
                                        // bank change, so this is not per region
    int regionContradictions;           // words inside a region marked written, taken or
                                        // patched: where a transform would corrupt a working
                                        // program on the author's own say-so
    int regionFindings[OPTREG_COUNT];   // live findings, by where their pattern sits

    // The nooptimize spans, kept as the regions are, in their own list.  A word
    // may be in a span and a region at once.
    OptRegionP handsOffP;               // every span declared, in source order
    OptRegionP handsOffTailP;
    int handsOffCount;                  // how many
    int handsOffWords;                  // emitted words inside any span

    // The speed regions and 'inline' markings: the author's declaration that a
    // LENGTH-CHANGING rewrite may happen, which nothing else, -O2's guess
    // included, gives.
    OptRegionP speedP;                  // every speed region declared, in source order
    OptRegionP speedTailP;
    int speedCount;                     // how many
    int speedWords;                     // emitted words inside any of them
    int speedBankWords[MAXBANK + 1];    // of those, per bank, as the regions do it
    OptInlineMarkP inlineMarksP;        // every 'inline NAME' marking, in source order
    OptInlineMarkP inlineMarksTailP;
    int inlineMarkCount;                // how many
    int inlineMarkResolved;             // of those, the ones that named a real routine
    int inlineMarkDuplicates;           // and the ones naming a routine already marked

    // Every '%%ceiling', in source order, and per bank the winning (lowest)
    // value, the first word no rewrite may reach, and its id; 0 when none.
    OptCeilingP ceilingsP;
    OptCeilingP ceilingsTailP;
    int ceilingCount;
    int bankCeiling[MAXBANK + 1];
    int bankCeilingId[MAXBANK + 1];

    // The findings, filled by optRunRules(), in bank then address order and never
    // re-sorted, so the report is byte-identical across runs.
    OptFindingP findingsP;              // live findings
    OptFindingP findingsTailP;          // the last of them, for appending
    int findingCount;
    int findingCounts[OPTRULE_COUNT];   // of those, per rule
    OptFindingP suppressedP;            // findings a precondition or a rule refused
    OptFindingP suppressedTailP;
    int suppressedCount;
    int suppressedCounts[OPTRULE_COUNT];
    int whyCounts[OPTWHY_COUNT];        // of those, per reason
    int savedWords;                     // words the live findings would remove
    int savedTemps;                     // storage words they would additionally free
    int savedTime;                      // microseconds removed from one pass through
                                        // every finding, per-hop savings excluded

    // The fate of every live finding, filled by optTransform().  The table keeps
    // the assembled values; this record is the only place a rewritten value
    // appears.
    OptXformP xformsP;                  // one per live finding, in finding order
    OptXformP xformsTailP;              // the last of them, for appending
    int xformRan;                       // non-zero once the fates have been decided
    int xformApplied;                   // non-zero when the edits were made (-O1),
                                        // zero for -O=xform's dry run without it
    int xformFates[OPTXF_COUNT];        // live findings, by fate
    int xformFreed;                     // pool words no instruction names once the
                                        // fired T8 words stop naming them: freed, and
                                        // NOT reclaimed -- they are still emitted
    int xformTime;                      // microseconds from one pass through every
                                        // fired word, per-hop savings excluded
    int xformHops;                      // fired T3 words, each worth one cycle per
                                        // hop actually taken
    int xformHopTime;                   // their per-hop savings, summed
    int xformThroughTaken;              // fired words whose read-through word has its address
    int xformThroughMaybe;              // taken, or may be written through a pointer:
                                        // measured, not refused
    int xformContradicted;              // fired words inside a region whose
                                        // declaration the evidence contradicts
    int xformRefusedInside;             // suppressed findings wholly inside a region
    // Bisection, set only when a modifier was given and the edits were made.
    int xformBisect;                    // non-zero when the selection ran
    int xformCandidates;                // records that would have fired: FIRED + OFF
    int xformOffBy[OPTBIS_COUNT];       // OFF records each modifier switched off; one
                                        // record may be counted under several

    // The inline sites a declaration reaches, filled by optTransform() when the
    // edits are made; empty, and printed nowhere, without a declaration.
    OptInlineP inlinesP;                // one per declared site, in bank then address order
    OptInlineP inlinesTailP;
    int inlineCount;
    int inlineFates[OPTIN_COUNT];       // of those, by fate
    int inlineSpent[MAXBANK + 1];       // words the fired copies were budgeted, per bank
    int inlineTime;                     // microseconds, the fired sites' best returns summed
    int inlineCandidates;               // records that would have fired, FIRED + OFF; apart
                                        // from xformCandidates, which counts T3 and T8 alone
    int inlineOffBy[OPTBIS_COUNT];      // OFF sites each modifier switched off, as xformOffBy
    int inlineRelaid;                   // relayout has run over the fired ones

    // The placement sites a declaration or -O2's switch reaches, filled by
    // optTransform() when the edits are made.
    OptPlaceP placesP;                  // one per recorded site, in bank then address order
    OptPlaceP placesTailP;
    int placeCount;
    int placeFates[OPTPL_COUNT];        // of those, by fate
    int placeTime;                      // microseconds, the fired sites' jumps summed
    int placeCandidates;                // records that would have fired, FIRED + OFF
    int placeOffBy[OPTBIS_COUNT];       // OFF sites each modifier switched off

    // The loops a declaration reaches, filled by optTransform() when the edits
    // are made.
    OptUnrollP unrollsP;                // one per recorded loop, in bank then address order
    OptUnrollP unrollsTailP;
    int unrollCount;
    int unrollFates[OPTUN_COUNT];       // of those, by fate
    int unrollSpent[MAXBANK + 1];       // words the fired unrolls were budgeted, per bank
    int unrollTime;                     // microseconds, the fired loops' savings summed
    int unrollCandidates;               // records that would have fired, FIRED + OFF
    int unrollOffBy[OPTBIS_COUNT];      // OFF loops each modifier switched off

    // The deleting rules' records, decided by planSpace() after the unrolls when
    // the edits are made.
    int spaceGated;                     // records planSpace() decided
    int spaceWords;                     // words the fired deletions would remove
    int spaceTime;                      // microseconds, their savings summed
    int spaceCandidates;                // records that would have fired, FIRED + OFF; apart
                                        // from xformCandidates, but switched off under
                                        // xformOffBy with T3 and T8
    int spaceMade;                      // deletions relayout made
    int spaceRelaid;                    // relayout has run over them

    // Pool words a declared T8 stops naming: poolFreeable of xformFreed may be
    // collected; poolReclaimed is what relayout dropped, lower when a slot is
    // still named under its key or its pool refused the edit.
    int poolFreeable;
    int poolReclaimed;
    int poolAdvice;                     // and the ones no declaration licensed, which stay
    int poolGrown;                      // words relayout's pool rebuild added: its splits less
                                        // its merges, negative when merges win

    // The -O2 guess, private to optguess.c; NILP until optBuildGuess() runs.
    struct optguess *guessP;

    // The extend-window analysis, private to optwindow.c; NILP until
    // optBuildWindow() runs.
    struct optwindow *windowP;

    // The eems and lems a declaration lets -O1 and -O2 delete, filled by
    // optPlanWindow() after the space deletions when the edits are made:
    // redundant and dead words first, then freed eems, each in address order.
    OptWinDelP windelsP;
    OptWinDelP windelsTailP;
    int windelCount;
    int windelFates[OPTWD_COUNT];       // of those, by fate
    int windelCandidates;               // records that would have fired, FIRED + OFF
    int windelOffBy[OPTBIS_COUNT];      // OFF records each modifier switched off
    int windelMade;                     // deletions relayout made
    int windelRelaid;                   // relayout has run over them
} OptTable, *OptTableP;

// optimizer.c.  Throughout, a function that names something returns a static
// string, never NILP.

// Accept one -O=modifier.  Returns 1 if known and applied, 0 if not known (the
// caller treats the command line as invalid).
int optimizeSetOption(char *nameP);

// Run the optimizer over a parsed program; rootP is the HEADER node.  The
// report goes to am1.c's outfP (<basename>.opt).  Returns 1 on success, 0 on
// failure, when am1.c removes the report.
int optimize(PNodeP rootP, char *basenameP);

// Build the word table.  Returns NILP if the tree holds something the builder
// cannot place (an address outside the bank, an unknown statement).
OptTableP optBuildTable(PNodeP rootP);

// One "%06o %06o" line per entry, the -T dump's format; reserved words omitted.
void optDumpTable(FILE *fP, OptTableP tableP);

// Release everything a table owns.  Safe to call with NILP.
void optFreeTable(OptTableP tableP);

const char *kindName(OptKind kind);

// optdecode.c

// Decode one value into decodeP; the spelling fields are left OPTS_NONE.
void optDecodeValue(int value, OptDecodeP decodeP);

// Fill decodeP's spelling fields from exprP, which may be NILP (OPTS_NONE).
void optClassifySpelling(PNodeP exprP, OptDecodeP decodeP);

// Decode one entry; a reserved word gets OPTG_UNKNOWN and OPTS_NONE.
void optDecodeWord(OptWordP entryP);

void optDecodeTable(OptTableP tableP);

const char *optGroupName(OptGroup group);
const char *optSpellingName(OptSpelling spelling);

// The -O=decode dump.
void optDumpDecoded(FILE *fP, OptTableP tableP);

// Returns 1 if the micro-op is in microBits (clf and stf share the flag
// field), 0 if not.
int optMicroOpPresent(unsigned int microBits, OptMicroId id);

// Store up to max of microBits' micro-ops in hardware order; returns how many.
int optOperateOrder(unsigned int microBits, const OptMicroOp **opsPP, int max);

// Apply an operate word's register micro-ops to ac and io in phase order (tw is
// the test word, pc what lap ORs in); flag operations and hlt are ignored.
void optSimulateOperate(unsigned int microBits, int ac, int io, int tw, int pc, int *acP, int *ioP);

// The -O=check self-test.  Returns 1 if every check passed, 0 if any failed.
int optDecoderSelfCheck(FILE *fP);

// optrefs.c

// Where cal lands; optrefs.c and optflow.c both need it.
#define CAL_WRITE_ADDR  0100    // cal deposits AC here (handbook page 18: cal is jda 100)
#define CAL_JUMP_ADDR   0101    // and continues here

// Build the reference edges, flags, code/data classification and conflicts of a
// decoded table.  Unresolved symbols are reported on stderr and skipped.
void optBuildReferences(OptTableP tableP);

const char *optRoleName(OptRole role);

// The -O=refs dump.
void optDumpReferences(FILE *fP, OptTableP tableP);

// Free one word's out list; the in lists share its edges.
void freeEdgeList(OptEdgeP edgeP);

// The word's first label, or "-" when it has none.
const char *firstLabelName(OptWordP entryP);

void writeReferenceReport(FILE *fP, OptTableP tableP, int bank);


// The report's list of words in conflict.
void writeConflictList(FILE *fP, OptTableP tableP);

// optflow.c

// Overlay the control-flow graph on a table whose references are built.
void optBuildFlow(OptTableP tableP);

const char *optFlowKindName(OptFlowKind kind);
const char *optBlockEndName(OptBlockEnd end);
const char *optSinkName(OptSink sink);
const char *optRecoveryName(OptRecovery recovered);
const char *optEntryCauseName(OptEntryCause cause);

// Returns 1 when a sink edge leaves through a routine's return word, which
// loses nothing because every call site holds its own edge back, 0 otherwise.
// Reads the routines, so it answers only after optBuildCallGraph().
int optSinkIsReturn(OptTableP tableP, OptBlockP blockP, OptFlowEdgeP edgeP);

// The -O=flow dump.
void optDumpFlow(FILE *fP, OptTableP tableP);

// Returns 1 when a word is in the control-flow graph (not reserved, not
// overlaid, classified code), 0 otherwise or for NILP.  The rules ask the same
// question, so they and the graph agree on what is an instruction.
int inGraph(OptWordP entryP);

// The first word emitted at bank and addr, or NILP.
OptWordP wordAt(OptTableP tableP, int bank, int addr);

// Sequence-break frames the program leaves in bank 0, 0 to OPTSBS_CHANNELS:
// none without an enable command, else those wholly below the start address,
// or all of them for 'stop', a start in another bank, or a start at 0100 or up.
int optSbsChannels(OptTableP tableP);

// The mnemonic when a word is esm, asc or isb in any form, else NILP.
const char *optSbsEnableName(OptWordP entryP);

// Emitted words that are enable commands, counted once per table.
int optSbsEnableCount(OptTableP tableP);

// The first enable command in emission order, or NILP.
OptWordP optSbsFirstEnable(OptTableP tableP);

// The "sbs enables:" evidence line of -O=flow and -O=calls.
void optDumpSbsEnables(FILE *fP, OptTableP tableP);

// Returns 1 when bank and addr is a frame's handler entry (4n+3), else 0.
int optIsSbsEntry(OptTableP tableP, int bank, int addr);

// Returns 1 for a skip-class word, one that may leave the next unexecuted; 0
// otherwise or for NILP.
int optSkipWord(OptWordP entryP);

// How a word ends a block, or OPTBE_COUNT when it is not a terminator.
// optreturn.c cuts its walk with it the way the graph will.
OptBlockEnd optTerminator(OptWordP entryP);

// Safe on a table whose flow was never built.
void freeBlockList(OptTableP tableP);

void writeFlowReport(FILE *fP, OptTableP tableP, int bank);

// The report's apparently-unreached section.
void writeUnreachedList(FILE *fP, OptTableP tableP);

// optreturn.c

// Classify every call site's return edge, before the blocks are cut.  A callee
// that cannot be read leaves OPTRC_UNKNOWN with a reason.
void optResolveReturns(OptTableP tableP);

const char *optReturnCaseName(OptReturnCase kase);
const char *optReturnReasonName(OptReturnReason reason);

// optroutine.c and optcallgraph.c

// Group the blocks into routines; called only from optBuildCallGraph().
void optBuildRoutines(OptTableP tableP);

// Build the routines, the call arcs, components, closure and cycles, and the
// sequence-break pool; after optBuildFlow(), before optRunRules().
void optBuildCallGraph(OptTableP tableP);

// Returns 1 when fromP reaches toP through resolved call arcs (a routine reaches
// itself only on a cycle); 0 otherwise, for NILP, or with no closure built.
int optRoutineReaches(OptTableP tableP, OptRoutineP fromP, OptRoutineP toP);

// The first owner of a block in routine-id order, or NILP when none owns it.
OptRoutineP optRoutineOfBlock(OptTableP tableP, OptBlockP blockP);

// Returns 1 when the routine's body holds the block, 0 otherwise or for NILP.
int optRoutineOwnsBlock(OptRoutineP routineP, OptBlockP blockP);

// Routines whose body holds the block, saturating at 255.
int optBlockOwnerCount(OptTableP tableP, OptBlockP blockP);

const char *optRoutineFormName(OptRoutineForm form);
const char *optReturnWordName(OptReturnWord word);

// The -O=calls dump.
void optDumpCalls(FILE *fP, OptTableP tableP);

// The report's recursion section: cycles of resolved call arcs only, nothing
// when there are none.
void writeCycleList(FILE *fP, OptTableP tableP);

// The return-word sharing measurement; fills callDepth and the tallies.  Called
// from optBuildCallGraph() once the components are classified.
void optMeasureSharing(OptTableP tableP);

// Safe on a table whose graph was never built.
void freeRoutineList(OptTableP tableP);

// optshare.c: advisory only; no finding, and no level reads it

// Build the sharing proposal, each group verified pairwise with
// optRoutineReaches(); called after optBuildCallGraph().
void optBuildSharing(OptTableP tableP);

// The report's return-word sharing section.
void writeSharingReport(FILE *fP, OptTableP tableP);

// The -O=share dump.
void optDumpShare(FILE *fP, OptTableP tableP);

// Safe on a table whose sharing was never built.
void freeShareGroups(OptTableP tableP);

// optregion.c: the author's declarations, checked against the evidence

// Clear the region state before the table builder's walk.
void optRegionReset(void);

// Take one region directive node as the builder's walk reaches it; bank and pc
// are where the next word would go.  The parser has already refused every
// malformed arrangement.
void optRegionStatement(OptTableP tableP, PNodeP nodeP, int bank, int pc);

// The region-overlay flags a word created now should carry.
unsigned int optRegionWordFlags(void);

// Record a newly added word against its region, if any; called for every entry.
void optRegionNoteWord(OptTableP tableP, OptWordP entryP);

// Check each region against the evidence and classify the live findings by
// region; after optRunRules().
void optCheckRegions(OptTableP tableP);

// The report's region section.
void writeRegionReport(FILE *fP, OptTableP tableP);

// The -O=regions dump.
void optDumpRegions(FILE *fP, OptTableP tableP);

// Where a finding sits: a word for the dump, a phrase for the report.
const char *optRegionPlaceName(OptRegionPlace place);
const char *optRegionPlaceText(OptRegionPlace place);

// Safe on a table whose source declared no region.
void freeRegionList(OptTableP tableP);

// The region a word is in, or NILP.
OptRegionP optRegionOfWord(OptTableP tableP, OptWordP entryP);

// The nooptimize span a word is in, or NILP.
OptRegionP optHandsOffOfWord(OptTableP tableP, OptWordP entryP);

// The hands-off report section and dump lines; nothing without a span.
void writeHandsOffReport(FILE *fP, OptTableP tableP);
void optDumpHandsOff(FILE *fP, OptTableP tableP);

// The speed region a word is in, or NILP.
OptRegionP optSpeedOfWord(OptTableP tableP, OptWordP entryP);

// The 'inline' marking naming a routine, or NILP.
OptInlineMarkP optInlineMarkOfRoutine(OptTableP tableP, OptRoutineP routineP);

// The speed-declaration report section and dump lines; nothing without one.
void writeSpeedDeclReport(FILE *fP, OptTableP tableP);
void optDumpSpeedDecls(FILE *fP, OptTableP tableP);

// The ceiling report section and dump lines; nothing without a '%%ceiling'.
void writeCeilingReport(FILE *fP, OptTableP tableP);
void optDumpCeilings(FILE *fP, OptTableP tableP);

// opttransform.c: the rewrite under -O1 and -O2

// Accept -O1 or -O2; am1.c refuses any other digit.
void optimizeSetLevel(int level);

// The optimization level: 1 for -O1, 2 for -O2, 0 otherwise.
int optTransformLevel(void);

// Bisection, in optimizer.c.  -O=upto=N, -O=range=B:LO-HI and -O=off=FILE
// switch off rewrites that would fire, to find the one that breaks a build; a
// rewrite stays on only if every modifier keeps it.  Every fate is decided
// before any word is rewritten, so any subset is a valid program.  Addresses
// are the source's before relayout: the bank in decimal, the address in octal.

// Non-zero when a bisection modifier was given (am1.c requires -O1 or -O2).
int optBisectGiven(void);

// The OPTBIS_ bits switching off the ordinal-th (from 1) rewrite that would
// fire, at bank and addr; 0 keeps it.  Marks the -O=off lines naming it.
unsigned int optBisectSelect(int ordinal, int bank, int addr);

// Warn about -O=off lines that matched no rewrite; after every selection.
void optBisectWarnUnmatched(void);

// The modifiers as given, e.g. "-O=upto=5 -O=off=bad.txt".
const char *optBisectSpec(void);

// "upto", "range" or "off" for one OPTBIS_ bit.
const char *optBisectName(unsigned int bit);

// Relayout, in optrelayout.c: edits that change a program's length.
// -O=edits=FILE supplies test edits ("delete BANK ADDR", "move BANK FROM TO
// after ADDR"; the bank decimal, addresses octal); -O=relayout is the dump.

// Read one -O=edits file.  Returns 1 if accepted, 0 for an empty name; an
// unreadable file or a malformed line is fatal.
int optRelayoutReadEdits(const char *pathP);

// Non-zero when an -O=edits file was given, even an empty one.
int optRelayoutGiven(void);

// Apply the edits (the file's, then the fired rewrites') to the parse tree, lay
// the program out again and check it, dumping on dumpP unless NILP.  An edit it
// cannot prove safe is refused and undone; its own inconsistency is fatal.
// Runs after the report, on the table as assembled.
void optRelayout(OptTableP tableP, PNodeP rootP, FILE *dumpP);

// Decide every live finding's fate and, when apply is non-zero, rewrite the
// statements that fire (zero is -O=xform's dry run); after optCheckRegions().
// A rewrite that does not reproduce the value it must is fatal.
void optTransform(OptTableP tableP, int apply);

// The -O=xform dump headed by labelP; only fired lines when firedOnly is set.
void optDumpTransforms(FILE *fP, OptTableP tableP, const char *labelP, int firedOnly);

// A fate: a word for the dump, a phrase for the report.
const char *optXformFateName(OptXformFate fate);
const char *optXformFateText(OptXformFate fate);

// T3 chains followed to their end, shared by the rewrite and -O=speed's S3
// count.  A chain longer than OPT_MAXCHAIN is refused, never cut short: a cut
// chain is not the end, and a second run would follow it further.
#define OPT_MAXCHAIN    4096

typedef enum
{
    OPTCHAIN_END,           // the chain ended: the last word is not one T3 follows
    OPTCHAIN_STOPPED,       // the walk declined a word T3 could follow (a span, a guess)
    OPTCHAIN_CYCLE,         // the next word is already on the chain, or is the jmp itself
    OPTCHAIN_LONG           // OPT_MAXCHAIN hops and not ended
} OptChainEnd;

// Returns 1 when T3 would follow the word as an intermediate: a direct jmp in
// the graph, not spinning on itself, never patched, written, taken or possibly
// written through a pointer; 0 otherwise or for NILP.
int optChainFollowable(OptWordP wordP);

// Follow a T3 chain from jmpP; chainPP (OPT_MAXCHAIN words) must already hold
// the checked intermediate.  Stops at a word in a nooptimize span or marked by
// guessMask.  Sets *hopsP (at least 1) and *stopPP (NILP unless STOPPED).
OptChainEnd optChainWalk(OptTableP tableP, OptWordP jmpP, unsigned int guessMask,
    OptWordP *chainPP, int *hopsP, OptWordP *stopPP);

// Release the transform record, safe if the pass never ran.  Installed trees
// belong to the parse tree and are not freed.
void freeXformList(OptTableP tableP);

// The report's Transforms section, under -O1 and -O2 only.
void writeTransformReport(FILE *fP, OptTableP tableP);

// optvalue.c: a measurement only; its whole output is the -O=values dump.

// The -O=values dump; after optRunRules(), whose findings it reads.
void optDumpValues(FILE *fP, OptTableP tableP);

// optscratch.c: can the storage locals share words?  Advisory: no finding, and
// no level reads it.

// The -O=scratch dump.
void optDumpScratch(FILE *fP, OptTableP tableP);

// The report's scratch-word sharing section; its figures are -O=scratch's.
void writeScratchSharingReport(FILE *fP, OptTableP tableP);

// optguess.c: the heuristics -O2 uses in place of P5

// Build the guess and set every live finding's guess fields; after
// optCheckRegions(), before optTransform().
void optBuildGuess(OptTableP tableP);

// The -O=guess dump.
void optDumpGuess(FILE *fP, OptTableP tableP);

// The report's "What -O2 would assume" section.
void writeGuessReport(FILE *fP, OptTableP tableP);

// "H1" to "H6".
const char *optGuessName(OptGuessId h);

// What a rewrite assumes when the heuristic finds nothing, as a clause.
const char *optGuessAssumption(OptGuessId h);

// The heuristics that apply to a finding under -O2: OPTGH_INREGION when its
// whole pattern is in an optimize region, else OPTGH_AUTHORIZED; and
// OPTGH_LENGTH as well for a deleting rule.
unsigned int optGuessApplies(OptFindingP findingP);

// Safe on a table the guess never ran on.
void freeGuess(OptTableP tableP);

// The heuristics marking one word, as bits; 0 for none, NILP or no guess.
unsigned int optGuessWordBits(OptTableP tableP, OptWordP wordP);

// optwindow.c: the extend window.  Advisory everywhere but a declaration:
// no finding, and -O1 and -O2 act on it only inside an optimize or speed
// region.

// Follow the window state and what depends on it, classify every eem and lem,
// and warn on stderr about each proved hazard; after optBuildGuess().
void optBuildWindow(OptTableP tableP);

// Record and decide every declared redundant, dead or freed word, after the
// space deletions are planned; only when the edits are made under a level.
void optPlanWindow(OptTableP tableP);

// After bisection: switch off a freed eem one of whose lems was switched off.
void optWindowSettleOff(OptTableP tableP);

// The report's section for the deletions, after relayout; nothing without one.
void writeWindowDeleteReport(FILE *fP, OptTableP tableP);

const char *optWinDelFateName(OptWinDelFate fate);
const char *optWinDelKindName(OptWinDelKind kind);

// opttransform.c, for optPlanWindow(): whether another fired rewrite makes,
// copies, moves or deletes the word, or deletes the one or two before it (a
// deleted word after a skip would change what the skip passes over).
// Returns 1 if one does, 0 if none does.
int optWordTouched(OptTableP tableP, OptWordP wordP);

// And whether the word is one statement's whole expression, rewritable in place.
int optWordRepresentable(OptWordP entryP);

// The -O=window dump.
void optDumpWindow(FILE *fP, OptTableP tableP);

// The report's extend-window section; nothing when no eem or lem is reached and
// nothing is unexamined.
void writeWindowReport(FILE *fP, OptTableP tableP);

// Safe on a table the analysis never ran on.
void freeWindow(OptTableP tableP);

// optspeed.c: the speed-mode candidates and their judges

// The -O=speed dump.
void optDumpSpeed(FILE *fP, OptTableP tableP);

// S1, inline expansion: the judge in optspeed.c, the settings in optimizer.c,
// the report in opttransform.c.

// Every block by id (index 0 unused); the caller frees it.
OptBlockP *optBlockIndex(OptTableP tableP);

// Judge one call site into *shapeP; the guess is recorded, not applied.
void optInlineJudge(OptTableP tableP, OptBlockP *blocksPP, OptWordP siteP, OptInlineShapeP shapeP);

// Returns 1 when the copy points this body word at the word after the copy (a
// return other than the dropped last word, or a jmp to it) and it can be
// rewritten, else 0.
int optInlineRetargets(OptInlineShapeP shapeP, OptWordP wordP);

const char *optInlineWhyName(int why);

// The notcopyable reason, whose shapeP->detail names a word.
int optInlineNotCopyable(void);

// The last word a rewrite may use in a bank: below its declared '%%ceiling',
// else 07750 in bank 0 (the read-in loader sits above) and 07777 elsewhere.
int optBankCeiling(OptTableP tableP, int bank);

// The declared ceiling as written, the first word no rewrite may reach; 0 if none.
int optBankCeilingDeclared(OptTableP tableP, int bank);

// -O=inline=cap:N (8 words by default) and -O=inline=reserve:N (64 per bank).
int optInlineCap(void);
int optInlineReserve(void);

// The report's inline section, after relayout; nothing without a site.
void writeInlineReport(FILE *fP, OptTableP tableP);

const char *optInlineFateName(OptInlineFate fate);

// S4, fall-through placement: the judge in optspeed.c, the switch in
// optimizer.c, the dump lines and report in opttransform.c.

// Returns 2 for a placement site (a direct, unpatched, unwritten jmp last in
// its reached block, whose target starts another block of the bank with this
// one as sole predecessor and is not .+1), 1 for a direct jmp whose target has
// other predecessors (context for -O=speed), 0 otherwise.
int optPlaceSiteKind(OptTableP tableP, OptWordP wordP);

// Judge a site (kind 2) into *shapeP; the guess is recorded, not applied.
void optPlaceJudge(OptTableP tableP, OptWordP jmpP, OptPlaceShapeP shapeP);

const char *optPlaceWhyName(int why);

// The afterskip reason: the guessed class, not a refusal.
int optPlaceAfterSkip(void);

// -O=undeclared (or its alias, -O=place=undeclared): given, as the
// author spelled it, and whether it licenses anything, which needs -O2.  Under
// the switch the deletions, the pool reclaim, the extend-window deletions and
// placement fire where no region declares them, on the guess.
int optUndeclaredGiven(void);
const char *optUndeclaredSpelling(void);
int optUndeclared(void);
// Non-zero for -O=reclaim=off (testing build): pool words are not reclaimed.
int optReclaimOff(void);

// Non-zero for -O=source: am1.c writes the program as am1 source after optimize().
int optSourceWanted(void);

// How -O=source writes the program, from optSourceMode().
#define SRC_MODE_TEXT       0   // an unchanged line copied from the source
#define SRC_MODE_RENDER     1   // testing: every line rendered, through the line map
#define SRC_MODE_TREE       2   // testing: every line rendered, with no line map
int optSourceMode(void);

// srccodegen.c: the program written back out as am1 source.

// Record one -D from the command line, for the output's header comment.
void srcNoteDefine(const char *defineP);

// Record one -I from the command line, for the output's header comment.
void srcNoteInclude(const char *pathP);

// Tie each statement to the source line it came from, before any rewrite;
// called by optimize() only for -O=source.
void srcMapLines(PNodeP rootP);

// Write the program in the tree as am1 source on outfP, naming sourceP in the
// header.  Returns 1 when every node was written, 0 when one was not (each is
// named on stderr and the output must not be used).
int srcCodegen(FILE *outfP, PNodeP rootP, const char *sourceP);

// Collect what the rewrites did to each statement, for the annotations; called
// by optimize() after relayout and before the table is freed.
void srcCollect(OptTableP tableP);

// Record a copy relayout made, from firstP to lastP, and what it is.
void srcNoteCopy(PNodeP firstP, PNodeP lastP, const char *whatP);

// Take each pool slot's first reference as parsed; called by optimize() before
// the transforms rewrite anything.
void srcSnapshotPools(void);

// The report's placement section, after relayout; nothing without a site.
void writePlaceReport(FILE *fP, OptTableP tableP);

const char *optPlaceFateName(OptPlaceFate fate);

// S2, loop unrolling: the judge in optspeed.c, the cap in optimizer.c, the
// dump lines and report in opttransform.c.

// Returns 1 when ispP is a loop latch: a reached, direct, unpatched isp before
// a direct, unpatched, unwritten jmp back no higher than it; else 0.
int optUnrollSite(OptTableP tableP, OptWordP ispP);

// Judge the loop a latch closes into *shapeP; the guess is recorded in
// guessBits and why.
void optUnrollJudge(OptTableP tableP, OptWordP ispP, OptUnrollShapeP shapeP);

const char *optUnrollWhyName(int why);

// The guessed reason, which the rewrite reports as its own fate.
int optUnrollGuessed(void);

// -O=unroll=cap:N: the most words a body may become, 64 by default.
int optUnrollCap(void);

// The report's unroll section, after relayout; nothing without a loop.
void writeUnrollReport(FILE *fP, OptTableP tableP);

const char *optUnrollFateName(OptUnrollFate fate);

// Space mode

// Returns 1 for a rule space mode carries (T1, T1b, T2, T6, T7, T13), else 0.
// T14 also deletes words but is not carried: its lia, lai and swp are PDP-1D
// instructions a machine may have switched off.
int optRuleDeletes(OptRuleId rule);

// The report's space and pool-reclaim sections, after relayout.
void writeSpaceReport(FILE *fP, OptTableP tableP);
void writeReclaimReport(FILE *fP, OptTableP tableP);

// optrules.c and optreport.c

// The largest spelling or evidence buffer.  A longer spelling is truncated; it
// is never parsed again, so that costs only readability.
#define OPTMSG_SIZE         1024

// Run every rule over a table whose flow is built, filling the finding lists.
void optRunRules(OptTableP tableP);

// One word's static time in microseconds (F-15D handbook), 0 for a word that
// is not an instruction.
int optWordTime(OptWordP entryP);

const char *optRuleName(OptRuleId rule);
const char *optRuleSummary(OptRuleId rule);
const char *optWhyName(OptWhy why);

// The -O=rules dump.
void optDumpRules(FILE *fP, OptTableP tableP);

// Release a finding list and the strings each finding owns.
void freeFindingList(OptFindingP findingP);

// optstrings.c.  The spell* calls write at most size bytes into bufP, the
// terminator included, truncating rather than overflowing.

// A freshly allocated formatted string, truncated at OPTMSG_SIZE and owned by
// the caller; out of memory is fatal.
char *allocPrintf(const char *fmtP, ...);

// A word as its source spelled it; without an expression, its symbol's name or
// a dash.
void spellWord(OptWordP entryP, char *bufP, int size);

// Just the operand, for a suggestion that changes only the mnemonic.
void spellOperand(OptWordP entryP, char *bufP, int size);

// The expression in the word's first [..] when it names an address symbol and
// no mnemonic.  Returns 1 with it in bufP, 0 with bufP empty.
int spellConstSymbol(OptWordP entryP, char *bufP, int size);

// The inner expression of exprP's first [..], or NILP: a T8 rewrite's source.
PNodeP optFirstConstInner(PNodeP exprP);

// Returns 1 when a leaf of the constant names anything but a literal, so its
// value may change when words move; else 0.
int optConstMayMove(PNodeP exprP);

// Operate micro-ops as am1 source in hardware order; none at all is nop.
void spellOperate(unsigned int microBits, char *bufP, int size);

// A skip word as source from its bits, for T2's inverted skip.
void spellSkipValue(int value, char *bufP, int size);

// The report's own sections, called from writeReport() in this order.

// The header notes; the table decides which P5 sentence is true.
void writeHeaderNotes(FILE *fP, OptTableP tableP);

// The findings by rule; a report with none still says so.
void writeFindingList(FILE *fP, OptTableP tableP);

// Refused patterns by rule, after a tally by reason.
void writeSuppressedList(FILE *fP, OptTableP tableP);

// Per-bank statistics, including each bank's reference and flow lines.
void writeStatistics(FILE *fP, OptTableP tableP);

#endif

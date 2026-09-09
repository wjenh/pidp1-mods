/* optimizer.h - the am1 optimizer's intermediate representation and entry points
 *
 * Purpose:
 *   Declares the word table that every optimizer analysis is built on, and the
 *   two calls am1.c makes into the optimizer: one to accept a modifier given
 *   after -O on the command line, one to run the optimizer after a successful
 *   parse.
 *
 * Architectural scope:
 *   The optimizer is an analysis-only advisor (see Optimizer/FeasibilityStudy.md,
 *   sections 5 and 7).  It runs after yyparse() has returned and before any of
 *   the code generators, reads the parse tree and the resolved symbol tables,
 *   and NEVER mutates either of them.  Everything it learns is kept in its own
 *   side tables, declared here, keyed by the parse node (PNodeP) or the symbol
 *   node (SymNodeP) that produced each word.
 *
 * Dependencies:
 *   am1.h for PNode, SymNode, BankContext, MAXBANK, BANKSIZE and WRDMASK.
 *   The caller must have included am1.h before this file.
 *
 * Execution model:
 *   Single threaded, one call to optimize() per am1 run.  The table is built
 *   once, consulted by the analyses (later tasks), and freed when optimize()
 *   returns.
 *
 * Revision history:
 *
 * 08-Sep-2026 claude - initial version, task A1: the word table
 * 08-Sep-2026 claude - task A2: instruction decode, source spelling, the
 *                      operate-group phase table
 * 08-Sep-2026 claude - task A3: reference edges, the reverse index, the
 *                      written/patched/taken/code/data flags and conflicts
 * 08-Sep-2026 claude - task A4: basic blocks, control-flow edges, the UNKNOWN
 *                      sink, reachability and the label/after-skip flags
 * 08-Sep-2026 claude - task A5: the preconditions, the rule engine, the
 *                      findings and the report they are written into
 * 08-Sep-2026 claude - task A6a: instruction code 12 is unused; the jfd
 *                      memory reference, its jump role, its block terminator
 *                      and its control-flow sink are gone
*/
#ifndef OPTIMIZER_H
#define OPTIMIZER_H

// What produced a word.  EXPR is every single-word statement whose content is
// an expression tree: instructions, data words and law immediates all look the
// same until the decoder (task A2) classifies them.  The other kinds are known
// from the statement type that emitted them.
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

// Per-word flags.  The low bits are set by the table builder; the analyses of
// later tasks add their own above OPTF_BUILDER_MASK.
#define OPTF_RESERVED       0x0001  // a 'table' word with no initializer: it occupies
                                    // memory but no value is emitted for it
#define OPTF_DUPADDR        0x0002  // another entry was emitted at the same bank and
                                    // address (an overlay, only reachable under -M)
#define OPTF_PCMISMATCH     0x0004  // the node's own pc differs from the running pc the
                                    // binary generator would use; a parser defect
#define OPTF_BUILDER_MASK   0x00FF  // every flag the builder owns

// Task A3: the flags the reference analysis derives from the edges.  The
// "maybe" flags are the conservative marks of study section 5.3: an indirect
// reference whose pointer cannot be resolved may reach any word whose
// address is taken, so every taken word in that bank is marked as possibly
// reached in the reference's role.  They are advisory; the definite flags
// come from edges alone.
#define OPTF_WRITTEN        0x00000100  // a write edge comes in (dap and dip included)
#define OPTF_PATCHED        0x00000200  // a dap or dip edge comes in: the address field varies
#define OPTF_TAKEN          0x00000400  // a taken edge comes in: the address is used as a value
#define OPTF_XCTTARGET      0x00000800  // an execute edge comes in
#define OPTF_READ           0x00001000  // a read edge comes in
#define OPTF_JUMPTARGET     0x00002000  // a jump edge comes in
#define OPTF_START          0x00004000  // the word at the program's start address
#define OPTF_CODE           0x00008000  // classified code (study 5.3)
#define OPTF_DATA           0x00010000  // classified data (study 5.3)
#define OPTF_MAYBE_READ     0x00020000  // possibly read through an unresolved pointer
#define OPTF_MAYBE_WRITTEN  0x00040000  // possibly written through an unresolved pointer
#define OPTF_MAYBE_ENTERED  0x00080000  // possibly jumped to or executed through an unresolved pointer
#define OPTF_ANALYSIS_MASK  0x000FFF00  // every flag the reference analysis owns

// Task A4: the flags the control-flow overlay derives.  HASLABEL and
// AFTERSKIP are set on every entry, in or out of the graph, because the
// preconditions that consult them (P1 and P4, study section 6) ask about a
// word's neighborhood and not about its role.  The other four are set only
// on words the graph holds.
#define OPTF_HASLABEL       0x00100000  // at least one label is defined at this word's
                                        // address; distinct from being a block start,
                                        // so P1 can see a label on an interior word
#define OPTF_AFTERSKIP      0x00200000  // the word one address lower in the same bank
                                        // decodes as a skip-class instruction, so this
                                        // word may be skipped over (precondition P4)
#define OPTF_BLOCKSTART     0x00400000  // the first word of a basic block
#define OPTF_ENTRY          0x00800000  // an entry point of the reachability walk
#define OPTF_UNREACHED      0x01000000  // in a block no entry reaches, and not xct'd
#define OPTF_XCTONLY        0x02000000  // in a block no entry reaches, but an xct edge
                                        // comes in: the word runs without being entered
#define OPTF_FLOW_MASK      0x0FF00000  // every flag the control-flow overlay owns

// The conflict set of study section 5.3.  A conflict is recorded, never
// resolved: these are the words a transform must leave alone and the ones
// the programmer may want to look at.  Patched is a kind of written, so
// OPTCF_CODE_PATCHED implies OPTCF_CODE_WRITTEN.
#define OPTCF_CODE_DATA     0x01    // classified both code and data
#define OPTCF_CODE_PATCHED  0x02    // code whose address field is written by dap or dip
#define OPTCF_CODE_WRITTEN  0x04    // code that is written at all

// A label attached to a word.  The symbol is the parser's own SymNode: a
// LOCATION or LCLLOCATION label, a variable name, or a constant-pool entry.
// The list is singly linked and owned by the entry it hangs from.
typedef struct optlabel
{
    struct optlabel *nextP;
    SymNodeP symP;
} OptLabel, *OptLabelP;

// ---------------------------------------------------------------------------
// Task A2: the instruction decode and the source-spelling classification.
//
// Every word that has a value gets a decoded view of its bits, whatever the
// word was meant to be.  The decoder does NOT decide code versus data (that
// is task A3); it says what the bits would do if the machine fetched them as
// an instruction, and separately how the source spelled the word.
//
// Bit positions follow the F-15D handbook, page 9 ("Instruction Format"):
// bits 0-4 are the instruction code, bit 5 is the indirect-address bit in the
// memory reference group and a group-specific modifier everywhere else, and
// bits 6-17 hold the address or the augmented instruction's variations.  Bit
// 0 is the most significant bit of the 18-bit word, so bit 5 is 010000 and
// bits 6-17 are 07777.  The mnemonic values are those of permsyms.def, which
// agree with the handbook's numerical list (page 67) wherever both have the
// instruction.
// ---------------------------------------------------------------------------

// The instruction group the top bits select.  Codes 00, 12, 14 and 36 are
// spare on every PDP-1 (handbook page 66, note).  Code 74 is spare on a plain
// PDP-1 and the special operate group on a PDP-1D; per the owner's decision
// (study section 9) the PDP-1D decode is always on, so 74 is always OPTG_1D.
// Code 12 decoded as a memory reference named jfd until 8-Sep-2026, when
// permsyms.def dropped jfd and the owner ruled the code unused; it is spare
// like the other three now.
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

// How the source spelled the word, from the shape of its expression tree.
// This is evidence for task A3's code/data classification, not a verdict:
// a dispatch table full of "jmp foo" words is spelled MNEMONIC_OPERAND and is
// still data.
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

// Skip group condition bits, bits 6-17 of a code 64 word (handbook pages 20
// and 21).  Each condition means "skip the next word if ...", and combining
// conditions skips if ANY of them holds (page 20, "the inclusive OR of the
// separate skips").  Bit 5 reverses the whole sense: the word then does NOT
// skip when the combined condition holds (page 20: sza with bit 5 set "becomes
// Do Not Skip on Zero Accumulator").  The sense switch and program flag
// fields select switch or flag 1 to 6, or 7 for all of them.  OPTC_SNI is the
// PDP-1D "skip if IO is not zero"; szi, 654000, is sni with bit 5 set, so it
// skips when IO is zero (owner-confirmed value, 08-Sep-2026).
#define OPTC_SNI        0004000     // PDP-1D: skip if IO is not zero
#define OPTC_SPI        0002000     // skip if IO is positive (sign bit zero)
#define OPTC_SZO        0001000     // skip if the overflow flip-flop is zero
#define OPTC_SMA        0000400     // skip if AC is negative (sign bit one)
#define OPTC_SPA        0000200     // skip if AC is positive (sign bit zero)
#define OPTC_SZA        0000100     // skip if AC is plus zero
#define OPTC_CONDMASK   0007700     // the single-bit conditions above
#define OPTC_SZSMASK    0000070     // sense switch number, 1 to 6, 7 = all
#define OPTC_SZFMASK    0000007     // program flag number, 1 to 6, 7 = all

// Shift group fields, code 66 (handbook page 9 for the layout, pages 18 and
// 19 for the instructions).  Bit 5 is the direction, bit 6 shift (arithmetic)
// against rotate, bits 7 and 8 the registers, bits 9 to 17 the step count as
// a number of one bits ("rar 1 = 671001", "rar 9 = 671777").
#define OPTSH_ARITH     0004000     // bit 6: one = shift, zero = rotate
#define OPTSH_REGS      0003000     // bits 7 and 8: 01 = AC, 10 = IO, 11 = both
#define OPTSH_AC        0001000
#define OPTSH_IO        0002000
#define OPTSH_COUNT     0000777     // bits 9 to 17, one bit per step

// Operate group micro-op bits, bits 5-17 of a code 76 word (handbook pages 21
// and 22; the three PDP-1D bits from permsyms.def).  These ARE the hardware
// bits, so a word's micro-op set is simply (value & OPTM_ALLBITS).  Note that
// permsyms.def spells lat as 762200 and lap as 760300: both carry the cla bit
// as well, exactly as the handbook says they are "usually combined".  The
// flag field (bits 14-17) is one four-bit slot: 0001 to 0007 clear flag n,
// 0011 to 0017 set flag n; a word cannot clear one flag and set another.
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

// In-out transfer fields (handbook page 22).  Bit 5 waits for the device's
// completion pulse; bit 6 says whether a completion pulse will come at all
// (it will when bit 6 differs from bit 5); bits 7-11 further qualify the
// device selected by bits 12-17.  Beyond that the decoder treats an iot as
// opaque: it names the word when its value is exactly one of the iot
// mnemonics in permsyms.def, and calls it "iot" otherwise.
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

// The hardware phase an operate micro-op is applied in.  From the handbook,
// page 21: "The instruction opr 3200 will clear the AC, put TW to AC, and
// complement AC" -- the clear happens first, the transfer into AC second, the
// complement last, which is why cla cma in one word (761200, the classic clc)
// leaves all ones while cma then cla as two words leaves zero.  The full
// timing, including where the PDP-1D micro-ops fall, is the emulator's
// (src/blincolnlights/pdp1/pdp1.c, read on the owner's direction 08-Sep-2026,
// and recorded in Claude/skill-updates/pdp1-operate-timing.md): the clears
// happen at time pulse 7, the complement of IO, the ORs into AC and the flag
// operation at TP8, the complement of AC and the halt at TP9, and the lai/lia
// transfer is armed at TP8 and completes during the NEXT instruction's fetch
// (TP0 to TP2), as a single exchange when both bits are set.  So the PDP-1D
// transfers come after everything else in the word, and their source
// registers are read after cla, cli, cmi and cma have acted.
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

// What a micro-op reads and writes, for the merge rules of task A5.
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

// ---------------------------------------------------------------------------
// Task A3: reference edges.
//
// Every symbol a word's expression names becomes an edge from that word to
// the word (or words, at an overlaid address) at the symbol's bank and
// address, with a role that says what the reference does to its target.
// The role comes from the decoded opcode (task A2) and from where the symbol
// sits in the expression: in the address field of a memory reference
// mnemonic it does what the mnemonic does; anywhere else (a bare data word,
// a law operand, inside a [..] constant, in arithmetic such as foo+1, in a
// table or var initializer) the address is merely being used as a value and
// the edge is "taken".  A memory reference whose address field is not a
// single symbol (lac foo+1, lac 100) still gets its edge, to the decoded
// address, with no symbol and the OPTEF_IMPLICIT mark.  Edges are also made
// for references the source does not spell: the word jda deposits AC into
// and the word after it that jda enters, and locations 100 and 101 for cal.
//
// Every word keeps the list of edges leaving it (outP) and the list of edges
// entering it (inP, the reverse index of study 5.3), plus a count of the
// entering edges per role, so "who writes this word" is a constant-time
// question and "is this word written" a constant-time answer.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Task A4: basic blocks and reachability (study section 5.4).
//
// The graph is overlaid on the words task A3 classified as code, leaving out
// the ones nothing can be said about: a reserved table word has no value, and
// an overlaid address (OPTF_DUPADDR) holds more than one word, so which of
// them the machine would fetch is not a question this analysis can answer
// (study 5.1 calls overlaid addresses opaque).  A4 does NOT revise A3's
// classification; "unreached" is a flag laid on top of "code", exactly as the
// task's deliverable 3 says, so that A3's dump and its expectations still
// hold.
//
// Every edge that leaves the graph -- through a pointer that could not be
// followed, a patched address field, an address nothing was emitted at, a
// word that is not code, an overlaid address, or DEBREAK's return to an
// interrupted program -- goes to one shared UNKNOWN sink, represented by a
// NILP target block and an OptSink reason.
// A sink edge carries no reachability: it says the analysis lost the thread,
// not that the thread ended.
// ---------------------------------------------------------------------------

// Why an edge goes to the UNKNOWN sink instead of to a block.
typedef enum
{
    OPTSK_NONE,         // not a sink edge
    OPTSK_INDIRECT,     // the instruction indirects and A3 could not follow the pointer
    OPTSK_PATCHED,      // a dap or dip writes the address field: the target varies
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

// How a block ends.  The first four are the terminator instruction that ended
// it; the last three mean the block ran into something rather than ending of
// its own accord.
typedef enum
{
    OPTBE_SKIP,         // a skip-class word: skip group, isp, sad, sas
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
    OPTEN_TAKEN,        // its address is used as a value somewhere, so an indirect
                        // jump or an xct could reach it (study 5.4)
    OPTEN_EXPORT,       // it carries an exported label: another program may enter it
    OPTEN_SBS,          // it is a sequence-break handler entry: bank 0, address 4n+3
    OPTEN_COUNT
} OptEntryCause;

// The sequence-break frame.  Each channel owns four words of low core starting
// at address 0, so a sixteen-channel system uses 0 through 077 and that is why
// programs conventionally begin at 0100 (project owner, 08-Sep-2026).  Within
// a channel's frame the hardware stores the pre-break AC, the packed PC word
// and IO in the first three words and begins EXECUTING at the fourth, so the
// handler entry of channel n is 4n+3, and channel 0's is address 3 (the
// pdp1-emulator skill, references/sbs.md, "The break sequence").  The
// single-channel SBS256 mode -- the only mode any shipped configuration
// enables -- uses channel 0's frame whatever the requesting device, so in
// practice only address 3 matters; the other fifteen are covered because they
// cost nothing and a program that never sets up a frame emits nothing there.
#define OPTSBS_CHANNELS     16      // channels in the full Type 20 system
#define OPTSBS_FRAMESIZE    4       // words of low core per channel
#define OPTSBS_ENTRYSLOT    3       // the word of a frame the hardware executes

// The address field of the DEBREAK instruction, "jmp i 1".  Recognized only
// in bank 0, which is where the hardware recognizes it (sbs.md, "DEBREAK").
#define OPTDEBREAK_ADDR     1

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
    const char *fileP;      // the source file the word came from: the command line
                            // source until a FILENAME statement (one of cpp's line
                            // markers) names another, and that file's name after
                            // it (task A5)
    int lineNo;             // source line the word came from, -1 if not known
    int index;              // position in emission order, 0 based
    unsigned int flags;     // OPTF_ bits
    struct optword *sameAddrP;  // next entry emitted at the same bank and address, NILP if none
    OptDecode decode;       // task A2: the decoded view and the spelling

    // Task A3: the edges and what they imply.  Out edges are in the order
    // the analysis found them (the expression's in-order walk, implicit
    // edges after the field's own, via-pointer edges last); in edges are in
    // their sources' emission order.
    OptEdgeP outP;          // edges leaving this word
    OptEdgeP outTailP;      // the last of them, for appending
    OptEdgeP inP;           // edges entering this word: the reverse index
    OptEdgeP inTailP;
    int outCount;           // edges leaving, OPTEF_NOWORD ones included
    int inCounts[OPTR_COUNT];   // edges entering, per EFFECTIVE role: an indirect
                                // edge counts as a read of the pointer word it lands
                                // on, and a placeholder edge does not count at all
    unsigned int conflicts; // OPTCF_ bits

    // Task A4: the block this word belongs to, NILP when the word is not in
    // the control-flow graph.  Membership is asked this way rather than with
    // a flag of its own, so the two can never disagree.
    OptBlockP blockP;
} OptWord, *OptWordP;

// ---------------------------------------------------------------------------
// Task A5: the preconditions, the rules and the findings (study sections 6
// and 7).
//
// A rule scans the words of one bank in address order and, wherever its
// pattern matches, produces a finding: what it found, what it suggests
// instead, and what that would save.  Before a finding is called live it has
// to pass the preconditions of study section 6 -- P1 no label or other side
// entry on an interior word, P2 no word of the pattern written or taken, P3
// no word an xct target, P4 no skip immediately before the pattern -- and
// whatever the rule itself demands (the operate phase order for T1, the
// summed shift count for T1b, and so on).  A pattern that matches and then
// fails one of those is NOT thrown away: it becomes a suppressed finding,
// carrying the reason, and the report prints it in its own section.  Study
// section 7 calls that half the more informative one.
//
// P5, that no location in the pattern is shared with a device or a sequence
// break handler, cannot be established from the source at all.  It is stated
// once in the report's header and is the reader's to check.
//
// The optimizer still never rewrites anything: a finding is a sentence in a
// file, and the words it names are left exactly as the program assembled them.
// ---------------------------------------------------------------------------

// The rules of the transform catalogue this phase implements.  The ids are
// the study's, and the order here is the order the report groups them in.
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

// Why a pattern that matched was refused.  The first five are the shared
// preconditions of study section 6; the rest belong to one rule each and say
// what about the words themselves made the rewrite unsafe.
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
    OPTWHY_COUNT
} OptWhy;

// The longest pattern any rule matches: T14's four-word exchange.
#define OPTFIND_MAXWORDS    4

// One finding, live or suppressed.  The words are the pattern in address
// order; the strings are this finding's own and are freed with it.
typedef struct optfinding
{
    struct optfinding *nextP;
    OptRuleId rule;
    OptWhy why;             // OPTWHY_NONE when the finding is live
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
} OptFinding, *OptFindingP;

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

    // Task A3: reference statistics, filled by optBuildReferences().
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

    // Task A4: the control-flow overlay, filled by optBuildFlow().  The
    // per-bank figures the report prints are recomputed there by walking the
    // block list, so nothing here has to be kept per bank.
    OptBlockP blocksP;              // every block, in bank then address order
    OptBlockP blocksTailP;          // the last of them, for appending
    int blockCount;
    int graphWords;                 // words the graph holds, the sum of wordCount
    int notCodeWords;               // words left out because A3 did not call them code
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
    int xctOnlyWords;               // words flagged OPTF_XCTONLY

    // Task A5: the findings, filled by optRunRules().  Both lists are built
    // in bank then address order and are never re-sorted, so the report is
    // byte-identical across runs of the same source.
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
} OptTable, *OptTableP;

// Accept one modifier given as -O=modifier on the command line.
// Returns 1 if the modifier is known and has been applied, 0 if it is not
// known, in which case the caller should treat the command line as invalid.
int optimizeSetOption(char *nameP);

// Run the optimizer over a successfully parsed program.
// rootP is the HEADER node; basenameP the source file's base name, used in
// the report heading.  The report is written to the FILE that am1.c has
// opened on its global outfP (the <basename>.opt file).
// Returns 1 on success, 0 on failure; on failure am1.c removes the report.
int optimize(PNodeP rootP, char *basenameP);

// Build the word table from the parse tree.  Exposed so later tasks and their
// tests can build a table without running the whole optimizer.
// Returns the table, or NILP if the tree contained something the builder
// could not place (an address outside the bank, an unknown statement).
OptTableP optBuildTable(PNodeP rootP);

// Print one line per emitted entry, "%06o %06o" of the combined bank/address
// and the value, exactly the format of the -T test dump.  Reserved table
// words are not printed, the -T dump does not print them either.
void optDumpTable(FILE *fP, OptTableP tableP);

// Release everything a table owns.  Safe to call with NILP.
void optFreeTable(OptTableP tableP);

// ---- task A2 entry points -------------------------------------------------

// Decode the bits of one 18-bit value into decodeP, clearing the structure
// first.  The spelling fields are left at their cleared values (OPTS_NONE).
void optDecodeValue(int value, OptDecodeP decodeP);

// Classify how an expression tree spells its word, filling the spelling
// fields of decodeP.  exprP may be NILP, which classifies as OPTS_NONE.
void optClassifySpelling(PNodeP exprP, OptDecodeP decodeP);

// Decode one table entry: its value, then its spelling from its expression.
// A reserved table word (no value) gets group OPTG_UNKNOWN and OPTS_NONE.
void optDecodeWord(OptWordP entryP);

// Decode every entry of a table.
void optDecodeTable(OptTableP tableP);

// Names for the report and the dump.  Both return a static string, never NILP.
const char *optGroupName(OptGroup group);
const char *optSpellingName(OptSpelling spelling);

// Print the decoded dump (-O=decode): one line per entry, reserved words
// included, in the format described at optDumpDecoded() in optimizer.c.
void optDumpDecoded(FILE *fP, OptTableP tableP);

// Is a micro-op present in a set of operate-group bits?  Handles the flag
// field, where clf and stf share bits.
// Returns 1 if present, 0 if not.
int optMicroOpPresent(unsigned int microBits, OptMicroId id);

// List the micro-ops present in microBits in the order the hardware applies
// them (by phase, then by order within the phase), at most max of them.
// Returns how many were stored.
int optOperateOrder(unsigned int microBits, const OptMicroOp **opsPP, int max);

// Apply an operate word's register micro-ops to AC and IO in phase order.
// tw is the test word switches and pc the value lap would OR in.  The results
// go to *acP and *ioP, masked to 18 bits; the flag operations and hlt change
// neither register and are ignored.
void optSimulateOperate(unsigned int microBits, int ac, int io, int tw, int pc, int *acP, int *ioP);

// The decoder's built-in checks (-O=check): the phase-table sanity anchors
// and a set of hand-computed decodes.  Prints one line per check to fP.
// Returns 1 if every check passed, 0 if any failed.
int optDecoderSelfCheck(FILE *fP);

// ---- task A3 entry points -------------------------------------------------

// Build the reference edges of a decoded table, follow constant pointers,
// apply the conservative marks for the pointers that could not be followed,
// then derive every word's flags, its code/data classification and its
// conflicts.  The table must have been decoded (optDecodeTable) first.
// Unresolved symbol references are reported on stderr and skipped; nothing
// here fails.
void optBuildReferences(OptTableP tableP);

// Name a role for the report and the dump.
// Returns a static string, never NILP.
const char *optRoleName(OptRole role);

// Print the reference dump (-O=refs): one line per entry, reserved words
// included, with the word's flags, its out edges and its in edges, in the
// format described at optDumpReferences() in optimizer.c.
void optDumpReferences(FILE *fP, OptTableP tableP);

// ---- task A4 entry points -------------------------------------------------

// Overlay the control-flow graph on a table whose references have been built
// (optBuildReferences) : set the per-word label and after-skip flags, cut the
// in-graph words into basic blocks, make the edges between them, walk
// reachability from the entry set, and flag the words nothing reaches.
// Nothing here fails; a program with no code words gets an empty graph.
void optBuildFlow(OptTableP tableP);

// Name a flow-edge kind, a block ending, a sink reason or an entry cause for
// the report and the dump.  All four return a static string, never NILP.
const char *optFlowKindName(OptFlowKind kind);
const char *optBlockEndName(OptBlockEnd end);
const char *optSinkName(OptSink sink);
const char *optEntryCauseName(OptEntryCause cause);

// Print the control-flow dump (-O=flow): the per-word flags, the block list
// with its successors, the entry set and the unreached list, in the format
// described at optDumpFlow() in optimizer.c.
void optDumpFlow(FILE *fP, OptTableP tableP);

// ---- task A5 entry points -------------------------------------------------

// Run every rule over a table whose control flow has been overlaid
// (optBuildFlow), building the live and suppressed finding lists in bank then
// address order.  Nothing here fails; a program with no code words gets no
// findings.
void optRunRules(OptTableP tableP);

// The static execution time of one word in microseconds, from the F-15D
// handbook's instruction list (see optWordTime() in optimizer.c for the
// numbers and their limits).
// Returns the time, or 0 for a word whose bits are not an instruction.
int optWordTime(OptWordP entryP);

// Name a rule, a refusal reason, or the one-line description of a rule, for
// the report and the dump.  All three return a static string, never NILP.
const char *optRuleName(OptRuleId rule);
const char *optRuleSummary(OptRuleId rule);
const char *optWhyName(OptWhy why);

// Print the rule dump (-O=rules): one line per finding, live then suppressed,
// in the format described at optDumpRules() in optimizer.c.
void optDumpRules(FILE *fP, OptTableP tableP);

#endif

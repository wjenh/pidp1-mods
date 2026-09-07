/* advmagic.h -- the drum-image magic numbers, single-sourced.
 *
 * Included by BOTH Adventure/adventure.am1 (am1 runs its source through
 * cpp) and the AdvDataLoader C tools, so every value here has to be
 * spelled identically in the two languages.
 *
 * That rules out bare integers. A bare numeric is DECIMAL to C and OCTAL
 * to am1, so a shared `31345` would silently mean two different numbers in
 * the two halves of the build. am1's own `0d` decimal prefix is not C, and
 * C's bare decimal is not am1. HEX is the one radix both spell the same
 * way, so:
 *
 *   Write every value in this file as 0x. Never bare, never 0d.
 *
 * Why this file exists: SAVE_MAGIC was hand-copied into three places
 * (adventure.am1, advdrumloader.c, advsave.c) and WIZCOM's marker into two,
 * under two different names. On 02-Sep-26 the objTake/objHereMsg save
 * records were added and SAVE_MAGIC was bumped in adventure.am1 only. The C
 * copies still held the old value, so advdrumloader stopped recognising the
 * block it was meant to preserve and zeroed every save -- the cross-process
 * RESTORE test failed with "THERE IS NO SAVED GAME." while the assembly
 * side was entirely correct.
 */
#ifndef ADVMAGIC_H
#define ADVMAGIC_H

/* Word 0 of the SAVE block: "a real save lives here", as opposed to an
 * uninitialised drum image.
 *
 * BUMP THIS ON ANY CHANGE TO THE SAVE RECORD LAYOUT. A save written under
 * the old layout is then refused rather than restored with garbage read out
 * of words the old writer never filled in. Most recently bumped 0x7A72
 * (31346) -> 0x7A73 (31347) because SAVE_STATE_WORDS became derived from
 * its own boundary symbols (curRoom-objTakePtr+1) instead of the stale
 * literal 0d56, which restored four words -- curRoom, lampOn, ea1On,
 * grateLocked -- to the state record and shifted every offset after it;
 * before that 0x7A71 (31345) -> 0x7A72 for TASK-FR31 N41's objDropSeq
 * record, and 0x7A70 (31344) -> 0x7A71 for FR16a's objTake/objHereMsg
 * records. The earlier values, and the tests that assert each one is
 * refused, are in Claude/SupportCode/testing/adventure_saveload_test.py.
 *
 * Bumped 0x7A73 (31347) -> 0x7A74 (31348) on 06-Sep-26 for TASK-FR32 S-7,
 * which added pirateSeen and piratePrevLoc (adven.f4's DSEEN(6)/ODLOC(6))
 * to the "Game state" block above objTakePtr. SAVE_STATE_WORDS is derived
 * from that block's endpoints so it grew on its own, but every offset after
 * it shifted by two words, which is exactly the layout change this marker
 * exists to refuse.
 *
 * Bumped 0x7A74 (31348) -> 0x7A75 (31349) on 06-Sep-26 for TASK-FR15 15a,
 * which REMOVED oysterNagPending from the "Game state" block. adven.f4
 * 857-858's own per-turn test, run beside the 860 sweep that now lives in
 * turnLoop, is the one-shot that word was carrying by hand. This is the
 * first bump that SHRINKS the record rather than growing it -- offsets
 * after it shift by one word in the other direction, which the marker
 * refuses exactly as it refuses a growth.
 *
 * 0x7A75 = 31349 decimal. */
#define SAVE_MAGIC      0x7A75

/* Word WIZCOM_BASE_OFFSET of the same track: the WIZCOM record's own
 * validity marker, same role for the WIZCOM block. adventure.am1 called this
 * WC_VALID until 02-Sep-26; that alias is retired, so the value now has
 * exactly one name everywhere.
 *
 * 0x39447 = 234567 decimal. */
#define WIZCOM_MAGIC    0x39447

#endif /* ADVMAGIC_H */

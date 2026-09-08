/* advmagic.h -- the drum-image magic numbers, single-sourced.
 *
 * Included by BOTH Adventure/adventure.am1 and the AdvDataLoader C tools,
 * so every value here has to be spelled identically in the two languages.
 * Write every value in this file as 0x. Never bare, never 0d.
 *
 * These markers must have exactly one definition each.
 */
#ifndef ADVMAGIC_H
#define ADVMAGIC_H

/* Word 0 of the SAVE block: "a real save lives here", as opposed to an
 * uninitialised drum image.
 *
 * BUMP THIS ON ANY CHANGE TO THE SAVE RECORD LAYOUT.
 * A save written under the old layout is then refused rather than restored with garbage.
 *
 * Change log, newest last:
 *   0x7A70 -> 0x7A71  added the objTake and objHereMsg records.
 *   0x7A71 -> 0x7A72  added the objDropSeq record.
 *   0x7A72 -> 0x7A73  SAVE_STATE_WORDS became derived from its own
 *                     boundary symbols (curRoom-objTakePtr+1) instead of
 *                     the stale literal 0d56, restoring four words --
 *                     curRoom, lampOn, ea1On, grateLocked -- to the state
 *                     record.
 *   0x7A73 -> 0x7A74  06-Sep-26, added pirateSeen and piratePrevLoc
 *                     (adven.f4's DSEEN(6)/ODLOC(6)) to the game-state
 *                     block above objTakePtr.
 *   0x7A74 -> 0x7A75  06-Sep-26, REMOVED oysterNagPending from the
 *                     game-state block; adven.f4 857-858's own per-turn
 *                     test, run beside the 860 sweep in turnLoop, is the
 *                     one-shot that word was carrying by hand.
 *   0x7A75 -> 0x7A76  07-Sep-26, released.
 *
 */
#define SAVE_MAGIC      0x7A76

/* Word WIZCOM_BASE_OFFSET of the same track: the WIZCOM record's own
 * validity marker, same role for the WIZCOM block.
 *
 */
#define WIZCOM_MAGIC    0x39448

#endif

```
+----------------------------------------------------------------------------+
|                           pidp1-mods                                       |
|                                                                            |
|        S K E T C H P A D   U S E R   I N T E R F A C E   G U I D E         |
|             FOR THE PDP-1 COMPUTER AND TYPE 340 VECTOR DISPLAY             |
|                  In the style of a classic DEC reference                   |
+----------------------------------------------------------------------------+
|  SYSTEM: 64K Words (16 Banks), Type 23 Drum, BBN Clock, Type 550 Microtape |
|  SOFTWARE: Version 2.2 (11-Nov-2026) by Bill Ezell & Claude Opus            |
+----------------------------------------------------------------------------+
```

---

## 1. The Lightpen Interface & Target States
The lightpen acts as the primary pointer device.
The tracking crosshair centers on the dot (**pseudo pen location**) which automatically snaps
and locks onto items via prioritized proximity routing.

* **Pen-Down / Touch:** Tracks pen input smoothly across canvas.
* **Pen Lift (on blank space):** Safe; terminates ongoing actions / gestures.
* **Pen Lift (on object):** Executes selections, draws bright alignment cross.
* **No-Motion (Hold 3-5 sec):** Alternative execution anchor for deep canvas actions.

### Selection Dynamics
* **Lock Priority Mechanics:** Point locks override adjacent line/arc segment locks.
* **Text/Instance Selection:** Aim directly at the geographical center point.
* **Menu Dismissal:** Lift pen from current item row, clear bounding path, or leave idle for 3 seconds.

---

## 2. Nested Submenu Logic Matrix

```
Bar:  Draw   Edit   Picture   Misc   Locate   Undo   Stop   View

Locate, Undo and Stop act at once (Undo is dim while there is nothing to undo).
The other five open a list; a row ending in ">" opens its sublist beside it.
Drive n shows the drive in use (Drive 1 at start). Show and Forces are
toggles, lit while on.

[Draw]          [Edit]          [Picture]   [Misc]          [View]
+-- Line        +-- Move        +-- Define  +-- Save        +-- x1
+-- Circle      +-- Delete      +-- Inst    +-- Load        +-- x2
+-- Truss >     +-- Constr >    +-- Copy    +-- Drive n >   +-- x4
|   +-- Beam    |   +-- Horv    +-- Clear   |   +-- D1      +-- x8
|   +-- Load    |   +-- Equal   +-- Tie     |   +-- D2      +-- Pan
|   +-- Roller  |   +-- Paral   +-- Desig   |   +-- D3      +-- Center
|   +-- Axis    |   +-- Perp    +-- Name    |   +-- D4
|   +-- Forces  |   +-- Length              |   +-- D5
|   +-- Stiff   |   +-- Mid                 |   +-- D6
+-- Text        |   +-- Fix                 |   +-- D7
+-- Dim >       |   +-- Show                |   +-- D8
    +-- Number  +-- Size >                  +-- Dir
    +-- Dist    |   +-- x1                  +-- Del cons
    +-- Change  |   +-- x2                  +-- Unfix
    +-- Gap     |   +-- x4                  +-- Library >
    +-- Size    |   +-- x8                  |   +-- Load
                +-- Scale >                 |   +-- Save
                |   +-- Part                |   +-- Dir
                |   +-- Figure              |   +-- Delete
                |   +-- All                 +-- Plot
                +-- Change >
                |   +-- Master
                |   +-- Cycle
                |   +-- Dwell
                |   +-- Seq
                +-- Dismem
                +-- Keep >
                    +-- x1/3
                    +-- x1/2
                    +-- x1
                    +-- x2
                    +-- x3
                    +-- Full
                    +-- Inch
```

---

## 3. Core Software Limits & Boundary Constraints

* **Points:** Max 256
* **Lines:** Max 256
* **Arcs:** Max 34
* **Instances:** Max 48
* **Masters:** Max 16 slots (M1 to M16)
* **Constraints (Total Slots):** Max 64
* **Constraints (Internal/Slot):** Max 16
* **Attachers / Slot:** Max 6
* **Designations:** Max 6
* **Nested Masters per Master:** Max 8
* **Texts (Canvas):** Max 64
* **Texts (Masters Total):** Max 64, costing up to 900 characters (each text two more than its
  characters)
* **Max Text Length:** 24 characters
* **CRT Max Glyphs:** 360 characters total visible
* **Numbers (Total):** Max 64
* **Vector Radii Boundaries:** 15 to 1294 display units
* **Display List Memory Pools:** Two volatile core storage lanes configured at
  **341 words** and **530 words**.
* **Master Tape Footprint:** Unified master tables must fit inside 23 blocks on a standard picture tape.
  Individual library master exports are capped at 32 tape blocks.

---

## 4. Geometric & Structural Constraints

* **Horv:** Locks targeted lines into strict vertical or horizontal orientation, selecting by which orientation
  is closest during initial pen lift.
* **Equal, Paral, Perp:** Dual-stage selections.
  Modifies layout based on mean geometric distances or coordinate cross-angles around component midpoints.
* **Length:** Preserves current line lengths explicitly against all upstream constraint engine recalculations.
* **Mid:** Maps a target point precisely to the midpoint of a line, forcing downstream vertex shifts to
  distribute evenly across all connected nodes.
* **Fix / Unfix:** `Fix` markers lock coordinates against relaxation solvers.
  `Misc > Unfix` drops all active instance anchors instantly across the picture.
* **Implicit Snapping:** Dropping points on raw lines or circular arcs auto-generates
  underlying **N** or **C** lock attributes inside the global table matrix.

---

## 5. Architectural Hierarchy (Masters, Instances, Copies)

```
MASTER (M1-M16)     Encapsulated drawing blueprint template.
|
+-- INSTANCE        Dynamic link pointer inside core display lane.
|                   Follows master updates globally. Rigid structure.
|
+-- PLACED COPY     Expanded raw vector collection.
                    Breaks structural links to permit individual node edits.
```

* **Dismem (Dismember):** Unpacks an active instance down one nesting level,
  matching coordinates exactly to current parent scaling properties.
* **Tie Points & Fastening:** Structural anchors (**Attachers**) link active elements.
  Overlapping shared tie points binds instances into a rigid, single body.
* **Motion Animation:** Controlled via `Change > Master`, `Cycle`, `Dwell` (50-9999ms),
  and `Seq`. Loops sequence frames predictably.

---

## 6. Physical Truss Analytics Engine
Calculates true internal stress points across structural frame nodes via iterative numeric approximation routines.

* **Beam:** Converts a plain vector segment into a rigid truss structural load-member.
* **Load:** Applies linear downward vector force equal to exactly 10x its drawn length.
* **Roller (O):** Grants free lateral movement along a set path, resisting force only across its perpendicular axis.
* **Axis:** Snaps a Roller's allowable travel orientation path parallel to a chosen guide line.
* **Stiff:** Sets user-defined elasticity attributes (0.1-99.9) for resolving redundant truss nodes.
* **Forces:** Overlays real-time numeric calculations directly onto vector middles.
  Pushing forces show compression (-), while pulling forces indicate standard structural tension (+).
* **System Flags:** Defective matrix setups output a `3-dash (---)` fault map alongside an explicit
  **UNSTABLE** screen indicator.

---

## 7. Global Constraint & Overlay Status Symbols
Activating `Edit > Constr > Show` displays the active contraints via circled indicators:

* **[H]** Horv Line
* **[E]** Equal Length
* **[P]** Parallel Line
* **[R]** Perpendicular
* **[L]** Fixed Length
* **[X]** Midpoint
* **[N]** Point-on-Line
* **[C]** Point-on-Arc
* **[F]** Fixed Position
* **[A]** Joint Attacher
* **[1 to 6]** Designated Point Index
* **[B]** Truss Beam
* **[W]** Structural Load
* **[O]** Support Roller / Number Gap
* **[D]** Dist Constraint
* **[Y]** Change Delta
* **[S]** Scale Group Ratio
* **[U]** Full Scale Max
* **[I]** Inch Dimensions
* **[Z]** Instance Match

---

## 8. System Status & Error Diagnostic Table

| Flag Code | Diagnostic Criteria / Root System Cause |
| :--- | :--- |
| **WAIT** | Microtape drive active; executing baseline storage track search operations. |
| **UNSTABLE** | Truss contains redundant pivot nodes or insufficient geometric boundary supports. |
| **NO ROOM** | Character arrays full, system coordinate lists saturated, or data windows exhausted. |
| **OFF EDGE** | Text run-out or geometric resizing forces nodes beyond display bounds. |
| **CONFLICT** | Newly entered manual scaling properties directly contradict existing group constraints. |
| **TOO BIG** | Drawing layout footprint exceeds structural limits or tape storage block allocation boundaries. |
| **TOO SLOW** | Execution tree too deep; calculation delays break display list generation loop bounds. |
| **CONSTR** | Point-to-point relational allocations exceed the maximum limit of 16 entries. |
| **LOOP** | Recursive error; parent template contains a child instance pointing back to itself. |
| **NESTED** | Library Save aborted; target master contains nested child sub-assemblies. |
| **BAD BLOCK** | Microtape transaction failed due to checksum errors or corrupted media tracks. |
| **DRUM** | System overlay retrieval routines failed; requires full drum-load reboot via 'ovlload'. |

---

## 9. Quick-Start: Operational Checklist

### Boot Code

cd /opt/pidp1-mods/FunStuff/Sketchpad\
make\
ovlload sketchpad.ovl\
or use *pdp1central*\
Program starts at address 100

### New Canvas
1. Tap screen on **INK** or press **Locate**.
2. Crosshair target centers automatically.
3. Select **Draw > Line** to begin chains.
4. Use **Locate** to break paths without losing current mode.

### Master Assembly Setup
1. Select **Picture > Tie**.
2. Mark interface attachment nodes (Max 6).
3. Select **Picture > Define** and pick target storage slot.
4. Input label characters via pen keyboard array; press **OK**.

---

## 10. Microtapes

  Microtapes must be mounted on one of the 8 drives before use.
  Use *mtp* or *pdp1central*.\
  Remember that this uses a correct simulation of the time an actual drive took,
  be patient.

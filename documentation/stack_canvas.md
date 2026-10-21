# The material stack canvas (spec)

A second presentation of the material graph: the material reads like a recipe, in user-named **sections** (Coordinates, Normal, Surface
color, Experiments, Output) that flow into **columns** left to right, each read top to bottom. The system places every step and owns every
route. It edits the same `xmaterial_graph::graph` through the same undo system as the old canvas, and every gesture is a console command, so an
AI drives it exactly as a person does.

Status: spec. `[A]` marks an assumption not verified in code. `[Q]` marks an open question for the owner.

```
source/Graph/xmaterial_stack.h              sections, the stack, serialize hooks              (new, no ImGui)
source/Graph/xmaterial_stack_adapter.h      v1 graph -> stack (stable topological sort)       (new, no ImGui)
source/Graph/xmaterial_step_names.h         step name -> GLSL identifier, shared with compiler (new)
source/Editor/xmaterial_stack_layout.h      passes A..F: placement, columns, Y, X              (new, pure)
source/Editor/xmaterial_stack_routes.h      highways, channels, lanes, route geometry          (new, pure)
source/Editor/xmaterial_stack_commands.h    stack commands and queries                         (new)
source/Editor/xmaterial_stack_canvas.h      drawing + gestures (ImDrawList, no node editor)    (new)
source/Editor/xmaterial_graph_widgets.h     param widgets moved out of xmaterial_graph_canvas.h (new, moved code)
```

## 1. Rules (decided)

- One model, several presentations: stack canvas, old canvas (read-only), console, compiler; later a text view.
- Semantic graph (nodes, connections) is separate from organisation (sections, their ordered step lists). No x,y, spine, column or order field
  per node. Coordinates are derived every layout.
- Every connection is stored. Adjacency never implies a connection: "next in the section" means "read next", nothing else.
- **Order invariant**: global order = sections in order, each section's steps in order. For every connection, producer index < consumer index.
  Every command keeps it; the adapter establishes it.
- Stable order: a new step goes where the user put it; nothing else moves in the order. Unrelated branches never reshuffle.
- Columns are layout only (E27 `NODE_SCRIPTING_DESIGN.md` §12.5). Which column a section lands in has no meaning.
- Sections do not make scope. Unlike E27 spines with owned `End` markers (§4.1) and the scope-depth rule (§11.5), any step can feed any later
  step. Scope arrives only with loop/switch regions (deferred), which would reuse E27's owned-marker model.
- Section ≠ collapsed section ≠ subgraph. A section organises. A collapsed section shows a boundary summary. Only subgraphs (later) define a
  reusable interface.
- Move ≠ insert. Moving changes presentation and keeps bindings. Inserting on a connection routes that connection through the new step.
  Moving to another section changes organisation only. Duplicating copies bindings explicitly.
- No auto-wiring by type. A binding is made only by an explicit pick. If several ports fit, the user chooses.
- The system owns every route: wires never cross steps, direction picks the side, distance picks the lane, one trunk per source output.
- Step names are identity for people (chips, console symbols, GLSL variable names). GUIDs stay the identity for storage and undo.
- Output section is an output stack (like Unity's Master Stack): it holds the output nodes, is always last, cannot be deleted. v1 is fragment
  only; the stage field is reserved.

## 2. Data model

### 2.1 Semantic graph (changed)

`xmaterial_graph::node` (`source/Graph/xmaterial_graph_node.h`) gains one saved field:

```cpp
std::string m_StepName = {};    // unique in the graph; chips, console symbols (@Name), GLSL names. "" until named (adapter/CreateNode name it)
// XPROPERTY_DEF: obj_member<"StepName", &node::m_StepName>      (shown and editable in Node Properties)
```

`node::m_Pos` stays as a transient member until the old canvas is removed: v2 files do not write it (`flags::DONT_SAVE`), the adapter reads it
from v1 files `[A: a DONT_SAVE member is still accepted on read, or an unknown path "node/Position/X" is skipped by the sprop reader — verify;
otherwise read v1 positions through a load-only lambda member]`. The old canvas fills it from the stack layout each layout pass.

No other node changes. Pins, sub-elements (`output_pin::m_SubElements`), `input_pin::m_ConnectionGUID`, `connection` are unchanged.

### 2.2 Organisation (new, `source/Graph/xmaterial_stack.h`)

```cpp
namespace xmaterial_graph
{
    using section_guid = xmath::strong_typing_direct_numerics_t<std::uint64_t, struct section_guid_tag>;
    // + section_guid_friend and reflected_type<section_guid>, same pattern as node_guid in xmaterial_graph_node_guid.h

    struct section
    {
        enum class kind  : std::uint8_t { USER, OUTPUT };
        enum class stage : std::uint8_t { FRAGMENT, VERTEX };      // VERTEX reserved: never created or shown in v1

        section_guid            m_Guid       = {};
        std::string             m_Name       = {};                 // unique among sections
        kind                    m_Kind       = kind::USER;
        stage                   m_Stage      = stage::FRAGMENT;     // meaningful for OUTPUT only in v1
        bool                    m_bCollapsed = false;
        std::vector<node_guid>  m_Steps      = {};                 // ordered; the only place order lives

        XPROPERTY_DEF
        ( "section", section
        , obj_member<"Guid",       &section::m_Guid,       member_flags<flags::DONT_SHOW>>
        , obj_member<"Name",       &section::m_Name>
        , obj_member<"Kind",       &section::m_Kind,       member_enum_span<kind_enum_v>,  member_flags<flags::DONT_SHOW>>
        , obj_member<"Stage",      &section::m_Stage,      member_enum_span<stage_enum_v>, member_flags<flags::DONT_SHOW>>
        , obj_member<"bCollapsed", &section::m_bCollapsed, member_flags<flags::DONT_SHOW>>
        , obj_member<"Steps",      &section::m_Steps,      member_flags<flags::DONT_SHOW>>
        )
    };
    XPROPERTY_REG(section)

    struct stack
    {
        std::vector<section> m_Sections;       // ordered; exactly one OUTPUT section, always last

        XPROPERTY_DEF( "stack", stack, obj_member<"Sections", &stack::m_Sections> )

        // Derived, rebuilt by Reindex() after every change (never saved):
        std::unordered_map<node_guid, std::pair<int,int>> m_Where;     // node -> (section index, step index)
        std::unordered_map<node_guid, int>                m_Global;    // node -> global order index
        void Reindex() noexcept;
        bool IsOrderValid(const graph& G, std::vector<connection_guid>* pViolations = nullptr) const noexcept;
    };
    XPROPERTY_REG(stack)
}
```

`graph` gains `stack m_Stack;` so `graph::serialize` and the document snapshot (`graph_document::Snapshot/Restore`, used by every
`graph_cmd` undo) carry it with no extra undo code. `[A: vector<section_guid/node_guid> serializes through sprop as "Steps[G:i]/Value" like
the existing pin guids — verify]`.

Comment nodes (prefab `groupWithComment`, `node::isCommentNode()`) are never steps. They stay in the graph for the old canvas; the stack hides
them (§8).

### 2.3 View state (never saved, never a command)

Selection (step, connection, chip focus), hover, trace mode, navigation history, scroll/zoom, lane memory (§5.5), column partition memory
(§4.4), the previewed step (§9.5).

### 2.4 Saved file (v2)

Same record sequence as today (`graph::serialize`, `source/Graph/xmaterial_graph.cpp:227`); `Info` gains a `Version` column, each node's
properties gain `node/StepName`, and a `StackLayout` record pair is appended last:

```
[ Info ]
{ nNodes:d  nConnections:d  Version:d }
//--------  --------------  ---------
     3            2             2

[ Node ]
{ Guid:G             PrefabGuid:G      }
//-----------------  -----------------
  #54E6882CBF886030  #952A9BAD60812EAA

< s32:d u32:g s16:C u16:H s8:c u8:h f32:f f64:F string:s wstring:S u64:G s64:D bool:c full_guid:GG enum:s >

[ xProperties : 21 ]
{ Name:s                                              Value:?                   }
//--------------------------------------------------  -------------------------
  "node/Name"                                         ;string "Vec2"
  "node/StepName"                                     ;string "UV"
  "node/Input Pins[]"                                 ;s64    2
  ...
// (two more Node + xProperties pairs: "DetailMap" (Texture), "Output: Target Color")

[ Connection ]
{ Guid:G             }
//-----------------
  #A1C0000000000001

[ xProperties : 2 ]
{ Name:s                                  Value:?                   }
//--------------------------------------  -------------------------
  "connection/InputPinGuid/Value"         ;u64    #1628203339ED8000
  "connection/OutputPinGuid/Value"        ;u64    #9628203339ED8003
// (second Connection pair)

[ StackLayout ]
{ nSections:d }
//-----------
      2

[ xProperties : 17 ]
{ Name:s                                         Value:?                   }
//---------------------------------------------  -------------------------
  "stack/Sections[]"                             ;s64    2
  "stack/Sections[G:0]/Guid/Value"               ;u64    #3B10F00D00000001
  "stack/Sections[G:0]/Name"                     ;string "Coordinates"
  "stack/Sections[G:0]/Kind"                     ;enum   "USER"
  "stack/Sections[G:0]/Stage"                    ;enum   "FRAGMENT"
  "stack/Sections[G:0]/bCollapsed"               ;bool   0
  "stack/Sections[G:0]/Steps[]"                  ;s64    2
  "stack/Sections[G:0]/Steps[G:0]/Value"         ;u64    #54E6882CBF886030
  "stack/Sections[G:0]/Steps[G:1]/Value"         ;u64    #77D2000000000002
  "stack/Sections[G:1]/Guid/Value"               ;u64    #3B10F00D00000002
  "stack/Sections[G:1]/Name"                     ;string "Output"
  "stack/Sections[G:1]/Kind"                     ;enum   "OUTPUT"
  "stack/Sections[G:1]/Stage"                    ;enum   "FRAGMENT"
  "stack/Sections[G:1]/bCollapsed"               ;bool   0
  "stack/Sections[G:1]/Steps[]"                  ;s64    1
  "stack/Sections[G:1]/Steps[G:0]/Value"         ;u64    #0F0F000000000003
```

Reading: `Version` missing (FIELD_NOT_FOUND) => v1; read it as a separate `Field` call, not in the `||` chain. After the connections:
`!isEOF() && getRecordName() == "StackLayout"` => read it, else run the adapter. `[A: getRecordName() is reliable right after the last
record's trailing ReadRecord() on EOF — check isEOF() first]`. Old builds reading a v2 file ignore the extra column and the trailing records
(columns match by name; the old loop stops after the connections) but lose positions (nodes at 0,0). `[A: confirm with one old build]`

### 2.5 xtextfile: what it can and cannot express

Checked in `D:\LIONant\xtextfile\source\xtextfile.h` and `implementation/xtextfile_inline.h`.

| Need | Status |
|---|---|
| 64-bit ids | yes, `G` (hex) |
| ordered lists of ids per section | yes, but one row per element: via sprop paths (above) or a flat record `{ Section:G Step:G }` |
| nested structures | **no native nesting**; records are flat tables. Nesting is only path strings (sprop) or parent-id columns |
| optional fields (columns) | partial: reading a column absent from the file returns `FIELD_NOT_FOUND` **and prints "Error: Unable to find the field"** (`xtextfile.cpp:2004`). Extra columns in a file are ignored |
| optional / unknown records | **no**: records are read strictly in order; `Record()` with another name returns `UNEXPECTED_RECORD` without consuming it, and `ReadRecord()` is protected, so a reader cannot skip an unknown record. New records can only be appended at the end |
| versioning | no file header/version; use a column (`Info.Version`) |
| count-less record | must hold exactly one row (`assert(C == 1)` on read) |

Asks for the owner (none block v1; the spec works around all of them):
1. `xerr stream::SkipRecord()` (public) so readers can skip unknown records (forward compatibility, records in the middle).
2. A quiet optional read: `FieldOptional(name, args...)` or a flag that turns `FIELD_NOT_FOUND` into "keep default" without the printf.
3. `[Q]` sprop paths make the layout verbose (~8 rows per section + 1 per step). Acceptable? The alternative is two flat records
   `[ Section : n ] { Guid:G Name:s Kind:s Stage:s Collapsed:c }` + `[ SectionStep : m ] { Section:G Step:G }`, hand-written like `Node`.
   Spec default: sprop (no new serializer code).

## 3. Order resolution

All in `xmaterial_stack.h`, pure functions over `graph` + `stack`.

- `StableTopoSort(order)`: Kahn's algorithm where the ready set is a min-heap keyed by the node's current index in `order`. Producers end up
  before consumers; everything already valid keeps its relative order. O((N+E) log N). Used by the adapter and by repair after a hand-edited
  file.
- `ValidRange(node, section)`: the step indices in `section` where `node` may sit: after its latest producer, before its earliest consumer
  (projected into that section; empty if the producer is in a later section or the consumer in an earlier one).
- `DefaultSlot(node)` (CreateNode without placement, orphans): right after its latest producer; else right before its earliest consumer;
  else the end of the last USER section (one is created, "Steps", if none).
- `ResolveMove(nodes, target)`: for a move that breaks the invariant, two candidate orders:
  - **dependents**: moving down past consumers drags those consumers (and transitively their violated consumers) to right after the moved
    block, keeping their relative order; moving up past producers drags producers (transitively) to right before it.
  - **nearest**: the valid slot closest to the drop (clamped into `ValidRange`; across sections, the nearest boundary slot that is valid).
  Both are previewed (§7.3); one undo step either way.
- Connect validation: the producer must already be before the consumer; else the same three options apply to the producer (move it up) or
  the consumer (move it down). Connect also gains a cycle check (it has none today, `xmaterial_graph_editing.h:174`).

## 4. Layout algorithm (`xmaterial_stack_layout.h`)

Pure function: `layout Build(const graph&, const stack&, float ViewWidth, const view_memory&)`. No ImGui calls except text measurement
through an injected `measure` callback, so it runs headless in the self-check. Rebuilt only when the document revision or the width bucket
changes (cache key: `graph_document` revision counter + `floor(ViewWidth/24)`).

### 4.1 Passes (order is required, as in E27 `NodeOS_UI_Canvas.h` Pass A:94 / B:143 / C:479)

| Pass | Does | Depends on |
|---|---|---|
| A step boxes | per step: width, height, visible rows | node, connections |
| B columns | partition sections into columns | A heights, width, memory |
| D routes | highways, channels, lanes in **discrete keys** | B, order |
| C Y | stack sections and steps per column; channel gaps grow with lanes | A, D channel lane counts |
| E X | column X from widest step + gutter lane extents | A, D gutter lane counts |
| F geometry | polylines, junctions, crossing gaps, slots, chips | C, D, E |

D runs before C/E because lanes are assigned on discrete keys `(column position, row)` that do not move when gaps grow: inserting space never
changes which intervals overlap, so lane assignment is independent of pixels and stable.

### 4.2 Pass A: step boxes

Rows, top to bottom: header (step name, then type name dimmed if different), one row per input (connected: source chip; unconnected: param
widget if `m_ParamIndex >= 0`, else the default text), one row per **used** output plus the first output always. Sub-element pins
(`x/y/z/w`) are never rows: a connection from a sub-element shows as `UV.x` on the consumer's chip. Width = max(text, widgets, 160), height =
`HEADER_H + rows * ROW_H + NODE_PAD_BOTTOM` (port geo constants from `NodeOS_CanvasSupport.h:230-257`). A collapsed section is one box:
header + summary line + one row per boundary input and output (§7.6).

### 4.3 Pass B: column wrapping

- Column width `CW` = widest step overall (uniform columns read best; `[Q]` per-column widths later?). Minimum gutter `G0 = 2*ICON_CLEARANCE +
  COLUMN_MARGIN`.
- `K = clamp(floor((ViewWidth + G0) / (CW + G0)), 1, nSections)`. Narrow window => one column.
- Partition the ordered sections into `K` contiguous runs minimising the tallest run (linear-partition DP, O(K·n²), n = sections). Sections
  never split. OUTPUT ends the last column.
- After E: if the real width (with lane extents) > ViewWidth and K > 1, K -= 1 and redo B..E (at most K times).
- Hysteresis: K changes only when ViewWidth crosses the threshold by > 24 px.

### 4.4 Stability of the column partition

Keep the previous partition (view memory) while K is unchanged and no column exceeds the tallest-run optimum by > 25%. A step added or a
section collapsed therefore never moves other sections between columns. Re-partition on K change, on request (`RepackRoutes -Columns`), or
past the 25% budget.

### 4.5 Pass C: Y

Per column: `y = TOP`; per section: header (`SECTION_HEADER_H`, holds the section's top slot), then each step at `y`, with the gap below it
`= NODE_GAP + max(0, channelLanes - 1) * LANE_GAP` (channel lanes from D), then the section's bottom slot, then `SECTION_GAP`. Port of
`spine_layout` + `GapRelY` (`NodeOS_UI_Canvas.h:98-141`), with the channel term added.

### 4.6 Pass E: X

Port of Pass C (`NodeOS_UI_Canvas.h:479-518`), but left to right from column 0 instead of root-outward (no root column here):
`X[0] = MARGIN + Extent(0,'L')`; `X[k+1] = X[k] + Extent(k,'R') + COLUMN_MARGIN + Extent(k+1,'L')` with
`Extent(c,s) = CW/2 + ICON_CLEARANCE + max(0, lanes(c,s)-1) * LANE_GAP` (`HighwayBaseOf`/`Extent`, lines 486-498). Steps are centred on
`X[c]` (as E27). `HighwayX` (line 521) is ported as is.

## 5. Highway routing (`xmaterial_stack_routes.h`)

### 5.1 Vocabulary

- **Highway**: vertical lanes beside a column, `H(c,'L')` and `H(c,'R')`. The gutter between columns c and c+1 holds `H(c,'R')` then
  `H(c+1,'L')`.
- **Channel**: horizontal lanes in a gap of a column: `CH(c,g)` where g = the gap after step g, or a section header/footer gap. Channels let a
  route cross a column without crossing a step.
- **Trunk**: all branches of one source **output** on one highway or channel. Key = (lane container, source output pin guid). A sub-element
  is its own output (`UV.x` and `UV.y` are two trunks). Different outputs never merge.
- **Key**: discrete vertical coordinate `(column position of the step, row index)`; compares lexicographically, maps monotonically to Y.

### 5.2 Route shapes (port of `OwnerColumnOf` 180, `LinkSide` 316, `LinkSides` 328, `PortSides` 341)

The order invariant means a consumer is always below its producer in the same column, or in a later column.

| Case | Route | Pin sides |
|---|---|---|
| same column (always downward) | out stub -> `H(c,'R')` down -> in stub | both on R (E27: down = R); the input glyph flips to the right edge for that link |
| next column, consumer lower (down-right) | stub -> `H(cp,'R')` down to the consumer's key -> horizontal across the gutter into the consumer | out R, in L |
| next column, consumer higher (up-right) | horizontal across the gutter at the producer's key -> `H(cq,'L')` up -> in stub | out R, in L |
| two or more columns apart | `H(cp,'R')` to the chosen channel of column cp+1 -> through `CH` -> jog on the next highway to the next channel -> ... -> `H(cq,'L')` to the consumer | out R, in L |

E27's rule kept: the vertical run lives in the source's column when right and down agree, else in the target's (`OwnerColumnOf`). A pin used
in several directions shows one glyph per side in use (`PortSides`/`SidesOf`, 341-365). Channel choice per intermediate column: the gap whose
**base** Y (Pass C with no channel growth) is closest to the straight line from producer to consumer; section header/footer gaps and the
gaps above the first / below the last step always exist. `[Q]` all same-column routes land on `H(c,'R')`, so left highways carry only up-right
arrivals; accept, or put same-column routes on L (input side)?

### 5.3 Lane assignment: full pack (port of lane packing, `NodeOS_UI_Canvas.h:395-472`)

Per lane container (each highway, each channel):
1. Build trunks: group branch spans by source output (E27 `span_group`, 431-445), interval = [min key, max key] over all branches.
   Channels: every haul spans the full channel width, so the interval is the column itself.
2. Sort trunks by span length ascending, then start key, then guid (deterministic). Short hops get the lane nearest the column; long spans go
   outer ("distance picks the lane").
3. Greedy interval partitioning: first lane with no overlapping interval of another trunk; else a new lane (E27 451-470).
4. Crossing minimisation: `C = Σ over stubs (horizontal segment from step edge to its lane at key y) of the number of other trunks in nearer
   lanes active at y`, plus, for gutter-crossing horizontals, the lanes of the far highway they pass. Repeat: swap adjacent lanes (whole lanes)
   if C drops, or move one trunk to another lane where it fits if C drops; stop when no move helps or after `4 * lanes` passes. Lane count
   never grows here.

### 5.4 Drawing junctions and crossings

- A trunk's vertical run spans its interval. Each branch leaves at its key. **Junction dot** where a branch leaves a trunk that continues past
  that point; a plain corner at the trunk's ends. Dots only ever join one source output with itself.
- **Crossing** (two different trunks): the vertical is drawn continuous; the horizontal is broken with a 4 px gap each side (screen space),
  meeting at a right angle. Never a dot at a crossing. Channel horizontals vs gutter verticals use the same rule.
- Wires never cross steps by construction (stubs only between a step edge and its gutter, horizontals only in gutters and channels). The
  self-check asserts no segment intersects a step rectangle.

### 5.5 Stability (lane memory)

View memory `map<trunk key, lane>` per container, cleared on load (so a load is a deterministic full pack).
1. Drop trunks that no longer exist.
2. Kept trunks, in previous (lane, start) order: keep the lane if it still has no overlap; else treat as new.
3. New trunks, short first: first free lane from 0 ("a new route takes a free lane").
4. If a container exceeds the budget (`MAX_LANES = 6` per highway or gutter width > `3 * COLUMN_MARGIN`; channels: 4 lanes) => full pack of
   that container only.
5. `RepackRoutes` (button + command) => full pack of everything (and `-Columns` re-partitions, §4.4).

`[Q]` lanes are not saved, so a reload may differ from what was on screen before saving. Persist a lane hint in StackLayout if that matters.

### 5.6 Emphasis

- Normal: every route drawn, dimmed (alpha 0.35, type colour `sType::m_Color`, 1.5 px).
- Selected connection (route click or chip select): the whole path from source to that consumer at full alpha, 2.5 px, on its own lanes
  (nothing moves); the other branches of the same trunk at 0.6.
- Selected step: its direct input routes and consumer routes lit.
- Trace upstream/downstream (menu or `U` / `D`): transitive closure lit, the rest dimmed to 0.15.
- Collapsed section: internal routes hidden; boundary routes end at the box rows.

## 6. Source chips

Every connected input row shows a chip with the source's name: `StepName` for a single-output step, `StepName.Output` for multi-output
(`Wetness.Roughness`), `StepName.x` for a sub-element. Chips never change with zoom or distance (no semantic zoom; no hiding).

| Gesture on a chip | Result | Command |
|---|---|---|
| hover | route + source step outline lit until hover ends | none (view) |
| click | select the connection (pins the route lit) | none |
| double-click / Enter / menu "Go to source" | scroll to and select the source; push nav history | none |
| Alt+Left / Mouse4 | back in nav history | none |
| menu "Find all uses" | popup of every consumer of that source output; pick = go there; "Replace all…" (§7.5) | none / group |
| menu "Replace source…" | search of compatible sources (§7.4), placed before the consumer | `Connect` (+ create group) |
| menu "Disconnect" / Del when focused | input falls back to its widget | `Disconnect` |
| Tab / Shift+Tab | focus next / previous chip of the selected step | none |

## 7. Interaction contract

Every gesture = exactly one undo step: one command, or `xeditor::RunGroup` (`dependencies/xeditor/include/xeditor/commands.h:72`). View
gestures issue no command. The canvas issues GUIDs only; symbols are for people and AI.

### 7.1 (+) slots (port of `DrawInsertMarker`, `NodeOS_UI_Canvas.h:1367-1481`; column-edge targets 1545-1610)

Slots: between every two steps, at the top and bottom of each section, and at column edges (before the first section of a column, after the
last). A column-edge slot means "a new section at this point of the section order" (columns are layout only). Each slot is selectable and
its (+) opens a menu:
- Add step… -> search of all prefabs; placed here; **no wiring** -> `CreateNode -Section -After/-Before -Name`.
- New section here -> `CreateSection` (+ `PlaceSteps` of the steps below the slot in the same section if "split here" is chosen).
- `[later]` Paste.

### 7.2 Gestures

| Gesture | Commands (one undo step) | Validation |
|---|---|---|
| (+) -> Add step | `CreateNode -Prefab -Node -Section -After\|-Before -Name` | none needed (no wiring) |
| drag from an output pin, drop on a (+) slot | search filtered to prefabs with a compatible input; ghost preview of the new step with the binding pre-selected; several fitting inputs => chooser; commit = group {`CreateNode`, `Connect`} | slot must be after the producer (slots before it are not offered) |
| drag from an output pin, drop on an input | `Connect` | type, cycle, order (§7.3 if violated) |
| drag from an input pin / chip "Replace source" | search: existing compatible outputs before the consumer + prefabs; a new step goes right before the consumer: group {`CreateNode`, `Connect`} | type, order |
| route menu "Insert step on this connection…" | search filtered by in-type = source type, out-type = consumer type; chooser if several ports fit; placed right before the consumer: `InsertOnConnection` | types |
| drag a step header onto a slot | `PlaceSteps -Nodes -Section -After\|-Before` | order (§7.3) |
| drag a section header onto a section slot | `MoveSection` | order: violations => same three options, applied to the whole section |
| step menu Duplicate | `DuplicateStep` (copies every input binding; outputs unconnected; placed after the original) | none |
| Del on a step | `DeleteNode` (also leaves its section; consumers fall back to widgets) | OUTPUT steps cannot be deleted from the stack |
| edit a param widget | `SetNodeProperty` (existing, value-based) | xproperty |
| rename (F2 / double-click the header) | `RenameStep` | unique, non-empty |
| collapse / expand a section | `SetSectionCollapsed` | none |
| select, hover, trace, go to, back, zoom, pan, preview | none | — |

### 7.3 Moves that break the order

Dropping where the invariant fails opens a popup at the drop point with three choices, each previewed (ghost of the resulting order and
routes while hovered): **Move dependents too**, **Nearest valid spot**, **Cancel**. The first two issue one `PlaceSteps` with
`-Resolve dependents|nearest` (the command computes the same orders, §3, so console and canvas agree). Cancel issues nothing.

### 7.4 Search popup

Opened by (+), pin drag, replace, insert-on-connection. Filters: port type (exact `type_guid`, the only compatibility the compiler knows
today, `IsPinCompatible` `xmaterial_graph_editing.h:66`), sub-elements of wider outputs (offered as `Name.x`), conversion prefabs whose input
matches (listed under "via conversion", e.g. `Vec4 To Vec2`), stage (OUTPUT: FRAGMENT only in v1). Existing sources are listed only if they
sit before the consumer. Text filter over prefab and step names. Nothing is wired until the user picks a port.

### 7.5 Replace a shared source

"Find all uses" -> "Replace all…" -> pick a new source (search) -> group {`Connect` per use}. The new source must be before the earliest use;
otherwise §7.3 options apply to the new source.

### 7.6 Collapsed section

One box: `Normal detail · 8 steps · in: UV, DetailMap · out: Normal`. `in` = distinct external sources feeding steps inside (chip text);
`out` = distinct inside outputs consumed outside. One row per boundary input/output; routes attach there. Expanding restores the same lanes
(lane memory keeps trunk keys; boundary trunks share the source output key).

### 7.7 Validation messages (shown as a tooltip at the drop; nothing in history)

`types differ (Vec3 -> Float); use a sub-element or a conversion` · `would make a cycle through <step>` · `<producer> is after <consumer>` (opens
§7.3) · `the output stack accepts only outputs` · `name already used`.

## 8. Migration adapter (`xmaterial_stack_adapter.h`)

Runs in memory when a file has no `StackLayout`; non-destructive (document not dirty; the first save writes v2).
1. Steps = instance nodes minus comment nodes.
2. Seed order: stable topological sort with ties broken by old position (`m_Pos.x`, then `m_Pos.y`): the old canvas read left to right.
3. Section hints: each comment node's rectangle (`m_Pos`, params `Size width`/`Size Height`) becomes a section named by its text (first line,
   40 chars; empty => `Group N`). A node belongs to the smallest rectangle containing its `m_Pos` `[A: m_Pos is the node's top-left in the same
   space as the comment's]`. Nodes in none: input nodes => `Inputs`, output nodes => `Output` (kind OUTPUT), the rest => `Steps`.
4. Section order: by the topological index of each section's first member; OUTPUT last. Concatenate, then re-run the stable topological sort.
   A section that is no longer contiguous splits into `Name`, `Name (2)` (deterministic; nothing lost).
5. Step names: prefab name, made a valid identifier, uniqued (`Texture`, `Texture2`).
6. Comment nodes stay in the graph (old canvas still draws them); the stack hides them. `[Q]` delete them once their section exists?

The old canvas becomes a read-only view over the same model and undo system (no drag, no link creation, no create menu; selection shared)
until the stack canvas passes the task tests (§11). Its positions come from the stack layout each layout pass (`m_Pos` transient).

## 9. Console commands

Run as `<resource name>\<Command>`. Wherever a node or pin is taken, a symbol also works: `@StepName`, `@StepName.Output`, `@StepName.Input`;
sections take a guid or `@"Section name"`. Symbols are resolved when the command runs; history stores the text as typed. New guids are always
supplied by the caller (as today) so redo stays valid; derived guids (pins of a new node, connections made by a composite command) come from
the new node's guid, like pin guids today.

### 9.1 Changed

| Command | Change |
|---|---|
| `CreateNode -Prefab -Node [-Section [-After <node> \| -Before <node> \| -First \| -Last]] [-Name]` | placement in the stack; without placement => `DefaultSlot`; `-X -Y` accepted, ignored once a stack exists; name defaults to the uniqued prefab name |
| `DeleteNode -Node` | also removes the node from its section; refused for the last output node |
| `Connect -Output -Input -Connection` | adds cycle check and order check (`<producer> is after <consumer>`); symbols |
| `MoveNode` | in stack mode returns `positions are derived; use PlaceSteps` |
| `ListNodes` | prints step name and section next to each node |

### 9.2 New (undoable; snapshot-based `graph_cmd` unless noted)

| Command | |
|---|---|
| `PlaceSteps -Nodes <g,g,...> -Section <s> [-After <node> \| -Before <node> \| -First \| -Last] [-Resolve none\|dependents\|nearest]` | move (presentation only; bindings kept). `none` (default) fails listing the violations |
| `InsertOnConnection -Connection <c> -Prefab <t> -Node <new> -Input <pin name> -Output <pin name> [-Name]` | create, route the connection through it, place it right before the consumer. Connection guids derived from `-Node` |
| `DuplicateStep -Node <n> -NewNode <new> [-Name]` | copy params and every input binding; placed after the original |
| `RenameStep -Node -Name` | unique; must sanitise to a non-empty identifier |
| `CreateSection -Section <new> -Name [-After <s> \| -Before <s>]` | empty section (never after OUTPUT) |
| `RenameSection -Section -Name`, `MoveSection -Section (-After\|-Before) <s> [-Resolve ...]` | |
| `DeleteSection -Section [-MergeInto <s>]` | refused if non-empty without `-MergeInto`; OUTPUT refused; never deletes nodes |
| `SetSectionCollapsed -Section -Collapsed 0\|1` | value-based |

### 9.3 New queries (no undo)

| Command | |
|---|---|
| `ListStack [-Section]` | sections in order; per step: guid, name, type, inputs as chips (`Base <- Wetness.Darken`), unconnected inputs with values |
| `Sources -Node` | each input: chip text, connection guid, source pin guid |
| `FindUses -Node [-Output <name>]` | every consumer input of that step (or one output) |
| `ValidatePlacement -Nodes -Section (-After\|-Before...)` | dry run: `ok`, or the violations and the order each `-Resolve` would produce |
| `LayoutStats` | columns, sections per column, lanes per highway/channel, crossings, segments crossing steps (must be 0), widest gutter, layout ms |
| `RouteInfo -Connection` | container chain, lanes, junctions, crossings on that route |
| `RepackRoutes [-Columns]` | view: full lane pack (and column re-partition) |
| `PreviewStep -Node [-Output] \| -Off` | view: the one intermediate preview (§10.4) |
| `SelectStep -Node`, `TraceRoute -Node (-Up\|-Down)` | view: so an AI can drive emphasis and screenshots |

## 10. Rendering

### 10.1 imgui-node-editor: do not use it for the stack canvas

It owns node positions (user drag, `ed::SetNodePosition` fights a computed layout), draws only bezier links (no polyline/lane API), sizes nodes
inside its own canvas, and its create/delete-link flow assumes free wiring. The stack needs computed placement, orthogonal routes with gaps,
slots between steps and chips. Draw it ourselves with `ImDrawList`, as E27 does:
- view transform: port `ToScreen`/`ToScreenLen` (`NodeOS_UI_Canvas.h:552-553`), wheel zoom anchored at the cursor (537-550), right-drag pan
  (615-636), the canvas background `InvisibleButton` pattern; anchor at column 0's left edge instead of a centred root column.
- widgets in steps: draw ImGui widgets at the screen position of their row (`SetCursorScreenPos`), reuse the existing param widgets
  (`DrawParamWidget`, `DrawTextureButton`, `DrawFileButton` in `xmaterial_graph_canvas.h:103-235`, moved to `xmaterial_graph_widgets.h`
  without their `ed::` calls). Zoom range 0.5-1.5; text scaled with `ImGui::SetWindowFontScale` `[A: acceptable quality; else clamp widgets to
  1.0 and scale only boxes]`.
- cull steps and routes outside the visible rect; layout is cached (§4), drawing is O(visible).
- keep imgui-node-editor only for the old read-only canvas.

### 10.2 Draw order

background -> section frames and headers -> routes (dimmed) -> steps -> chips -> lit routes (on top of dimmed ones, below steps) -> slots ->
drag ghost / previews -> popups.

### 10.3 GLSL names

`PinToVarName` (`source/Compiler/xmaterial_compiler.cpp:111-114`, today `"N_" + guid`) uses `StepVarName(node)` from
`xmaterial_step_names.h`: the step name made an identifier (letters, digits, `_`; leading digit or GLSL keyword => prefix `s_`), plus the
output name for multi-output steps (`wetness_roughness`); empty => `N_<guid>` as today. Uniqueness is guaranteed by step-name uniqueness. The
editor's error mapping (`xmaterial_editor.h:95`, matches the decimal guid in log lines) maps identifiers back through the same function.

### 10.4 One selected intermediate preview (first milestone)

`PreviewStep` writes a temporary copy of the graph where `Output: Target Color` takes the selected output (float -> `vec4(x,x,x,1)`, vec2 ->
`(x,y,0,1)`, vec3 -> `(xyz,1)`), compiles it as a side resource and shows it in Mesh Preview with a banner "Previewing Wetness.Roughness ·
Esc to stop". `[A: the compile path can build a temporary descriptor without touching the asset; unverified — if not, the preview task
becomes "compiler flag: preview output pin guid"]`.

## 11. Task tests

Run each by hand on the canvas and as a console script; record the measures from `LayoutStats`/`RouteInfo`/history. Every test also checks:
segments crossing steps = 0, order invariant holds, one undo step per gesture, undo then redo restores the identical file text.

| # | Test | Setup -> action | Pass | Measures |
|---|---|---|---|---|
| 1 | One UV feeding many branches | `UV` (Vec2) feeds 8 inputs over 3 sections | one trunk per container UV passes; junction dots, no parallel UV lanes; all 8 chips read `UV` | trunks for UV per highway = 1; total crossings; lanes per gutter |
| 2 | Independent normal and color branches | sections Normal, Surface color with no links between | no route between them; 1600 px => side by side, 500 px => one column | columns at 1600/500 px; cross-section routes = 0 |
| 3 | Multi-output step | `Wetness` outputs Roughness, Darken, each used twice | chips `Wetness.Roughness` / `Wetness.Darken`; two trunks, never merged | trunks = distinct outputs used (2); `FindUses -Output` correct |
| 4 | Insert tint into one branch | `Albedo` feeds Base color and Emission; insert `Vec4 Multiply` on Albedo->Base color | only that connection goes through the new step; chooser asked which input; Emission untouched | gestures ≤ 4; undo steps = 1; other connection guid unchanged; other steps' order unchanged |
| 5 | Move a step across a dependency | drag `Noise` below its consumer | popup with 3 previewed options; each gives a valid order; Cancel adds no history | undo steps = 1 (0 for Cancel); steps moved = exactly the dependents (option 1) / 1 (option 2) |
| 6 | Replace a shared mask | `Mask` used by 3 steps; Find all uses -> Replace all -> `Mask2` | 3 chips read `Mask2`; `Mask` still exists, unused | gestures ≤ 5; undo steps = 1 |
| 7 | Collapse a large section | 30-step section with 4 external inputs, 2 outputs | one box with summary; boundary routes end at it; expand restores identical lanes | column height before/after; lanes before = after expand; sections moved between columns = 0 |
| 8 | Change an instance value | edit a Float widget on an unconnected input | value changes, nothing moves | step rects diff = 0; lane diff = 0; undo steps = 1 |
| 9 | Invalid connection | Vec3 output onto Float input; producer below consumer; a cycle | type and cycle refused with the message, no history; order case opens §7.3 | history size unchanged for refusals |
| 10 | Add one node to a large material | 200 steps, 12 sections; (+) Add step in a middle section | only the steps below it in that column shift; no section changes column | lanes changed for unrelated trunks = 0; sections moved between columns = 0; layout ms (`[A]` budget 2 ms) |

The headless self-check (`xmaterial_stack_selftest()` in `xmaterial_stack_routes.h`, `assert`-based, run by a debug console command
`StackSelfTest`) builds tests 1, 3, 7, 10 from code and asserts: invariant, no segment through a step, one trunk per (container, output),
crossing count does not rise after the minimisation pass, lane memory unchanged for unrelated trunks after an insert.

## 12. Implementation tasks (in order; each one builds and leaves the editor usable)

Sized for one Sonnet session each. "Done" includes: builds, old canvas still works, `documentation/editor.md` updated for any command added.

1. **Stack model and file v2** — `Graph/xmaterial_stack.h` (new), `Graph/xmaterial_graph.h/.cpp`, `Graph/xmaterial_graph_node.h`,
   `Graph/xmaterial_graph_node_guid.h`. section, stack, `Reindex`, `IsOrderValid`, `node::m_StepName`, `Info.Version`, `StackLayout` read/write,
   `m_Pos` not written. Done: a v2 file round-trips byte-identical; every v1 material in `example.lionprj/Descriptors/Material` still loads.
2. **Order functions and adapter** — `Graph/xmaterial_stack_adapter.h` (new), hooked in `graph::serialize` read path. `StableTopoSort`,
   `ValidRange`, `DefaultSlot`, `ResolveMove`, comment hints, step naming. Done: all example materials open with a valid order, document not
   dirty; an assert self-check covers a cycle-free sort and both resolutions.
3. **Stack commands** — `Editor/xmaterial_stack_commands.h` (new), `Editor/xmaterial_graph_editing.h`, `Editor/xmaterial_editor.h`.
   `CreateNode`/`DeleteNode`/`Connect`/`MoveNode` changes, `PlaceSteps`, `RenameStep`, section commands, symbol resolution (`ResolveNodeArg`
   replacing `HexArg` for nodes/pins). Done: a console script builds test 4's graph by names; undo/redo of each restores the file text.
4. **Composite commands and queries** — same files. `InsertOnConnection`, `DuplicateStep`, `ListStack`, `Sources`, `FindUses`,
   `ValidatePlacement`. Done: tests 4, 6 and 9 pass from the console alone.
5. **Step names in codegen** — `Graph/xmaterial_step_names.h` (new), `Compiler/xmaterial_compiler.cpp` (`PinToVarName`),
   `Editor/xmaterial_editor.h` (error mapping). Done: the Shader panel shows `uv`, `detail_map` instead of `N_<guid>`; a forced compile error
   still marks the right node.
6. **Layout engine** — `Editor/xmaterial_stack_layout.h` (new, pure). Passes A, B, C, E with base gaps (no routes yet), partition memory and
   hysteresis. Done: `LayoutStats` prints columns/sections; resizing between 500 and 1600 px changes K only at thresholds.
7. **Router** — `Editor/xmaterial_stack_routes.h` (new, pure). Highways, channels, trunks, full pack, crossing minimisation, lane memory,
   geometry with junctions and gaps, `RouteInfo`, `RepackRoutes`, `StackSelfTest`. Done: self-check passes; segments through steps = 0 on
   every example material.
8. **Stack canvas: draw** — `Editor/xmaterial_stack_canvas.h` (new), `Editor/xmaterial_graph_widgets.h` (moved from
   `xmaterial_graph_canvas.h`), `Editor/xmaterial_editor.h` (panel "Material Stack" in the centre; old canvas as a read-only tab). Pan/zoom,
   section frames, steps with rows (no sub-pins), param widgets editing through `SetNodeProperty`, dimmed routes, step and route selection
   emphasis, trace. Done: tests 1, 2, 3, 8 pass visually and by measures.
9. **Source chips and navigation** — `xmaterial_stack_canvas.h`. Chip text, hover, select, go to source, back, find uses, replace,
   disconnect, keyboard focus. Done: every row of §6 works; test 6 by mouse.
10. **(+) slots, search, pin drag** — `xmaterial_stack_canvas.h`. Slots incl. column edges, slot menu, search popup with filters, ghost
    preview, port chooser, insert-on-connection from a route menu. Done: tests 4 and 9 by mouse; no gesture wires anything without a pick.
11. **Moves and collapse** — `xmaterial_stack_canvas.h`. Step and section drag onto slots, the §7.3 popup with previews, collapsed section
    box with boundary rows. Done: tests 5, 7, 10.
12. **Old canvas read-only** — `Editor/xmaterial_graph_canvas.h`. Positions from the stack layout, editing disabled, shared selection.
    Done: both panels show the same selection; no edit possible from the old one.
13. **One intermediate preview** — `xmaterial_stack_canvas.h`, `xmaterial_editor.h` (+ compiler only if §10.4's assumption fails).
    `PreviewStep`. Done: selecting a step and pressing P shows its output on the mesh; Esc returns to the material.
14. **Task test pass and docs** — run §11 by hand and as scripts, record measures in this file, update `documentation/editor.md` (stack
    panel, new commands). Done: all 10 pass; the owner decides when the old canvas goes.

## 13. Deferred (not in the first milestone)

Expression steps, the structured text view, always-live thumbnails and typed on-demand previews, loop/switch regions (E27 owned markers and
scope depth), semantic zoom, subgraphs with interfaces, pin modifier chains (`[Texture.R] -> OneMinus -> Clamp01`; sketch: modifier nodes
owned by an input, like E27's owned markers, promotable to steps by clearing the owner), VERTEX stage in the output stack, saved lane hints.

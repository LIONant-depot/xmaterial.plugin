# The material stack canvas (spec)

A second presentation of the material graph: the material reads like a recipe, in user-named **sections** (Coordinates, Normal, Surface
color, Experiments, Output), shown in **columns** by dependency depth, flowing left to right (§4). The system places every step and owns every
route. It edits the same `xmaterial_graph::graph` through the same undo system as the old canvas, and every gesture is a console command, so an
AI drives it exactly as a person does.

Status: spec. `[A]` marks an assumption not verified in code. `[Q]` marks an open question for the owner.

```
source/Graph/xmaterial_stack.h              sections, the stack, serialize hooks              (new, no ImGui)
source/Graph/xmaterial_stack_adapter.h      v1 graph -> stack (stable topological sort)       (new, no ImGui)
source/Graph/xmaterial_step_names.h         step name -> GLSL identifier, shared with compiler (new)
source/Editor/xmaterial_stack_layout.h      depth columns, order, Y, X                         (new, pure)
source/Editor/xmaterial_stack_routes.h      gutter lanes, route geometry                       (new, pure)
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
- Section ≠ collapsed section ≠ subgraph. A section organises. A collapsed section shows a boundary summary (one box, §7.6). Only subgraphs (later) define a
  reusable interface.
- Move ≠ insert. Moving changes presentation and keeps bindings. Inserting on a connection routes that connection through the new step.
  Moving to another section changes organisation only. Duplicating copies bindings explicitly.
- Type rule: a connection needs equal types, except that a float (or a sub-element such as `Vec2.x`) may feed a vec2/vec3/vec4 input, compiled as a splat `vec4(Vec2.xxxx)`; types of one GLSL type connect (`Color` and `Vector4`, both `vec4`); a longer vector feeds a shorter vector input, taking its first components (truncation, compiled `vec3(Albedo.xyz)`, shown on the chip, §6.5); a narrower vector into a wider input, float to int and vector to scalar are refused (a sub-element or a swizzle, §6.5, picks components). Colour spaces are guarded: an `SColor` (sRGB-encoded) feeds only `SColor` inputs and a `Color` (linear) never feeds an `SColor` input, refused with "SColor is sRGB-encoded; convert with 'SColor To Color'" / "Color is linear; convert with 'Color To SColor'" (material_contract.md §3). The rule is `AreTypesCompatible` (types alone) under `IsPinCompatible`, its message `TypesDiffer`, all in `xmaterial_graph_editing.h`.
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

Selection (step, connection, chip focus), hover, trace mode, navigation history, scroll/zoom, the previewed step (§9.5). The layout keeps no
memory (§5.5).

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

**Supersedes the section-flow columns** (sections partitioned into columns by height, each read top to bottom) per owner feedback
2026-10-21: columns now come from dependency depth, the flow is left to right, steps slide up and down to keep wires short. Sections stay
the reading order on disk and in every command; the canvas does not draw them (a depth column holds steps of several sections and a section
spreads over several columns, so frames would overlap; labelled groups can come back once they fit the layering).

Pure functions: `Layer(input, edges)` then the router's lanes, then `PlaceX`. No ImGui; text is measured by injected callbacks, so it runs
headless in the self-check. Rebuilt only when what it reads changes (steps, rows, widths, literals, edges); the view width plays no part.
Deterministic and memoryless: the same graph always gives the same picture, a load included.

### 4.1 Passes

| Pass | Does |
|---|---|
| A step boxes | per step: width, rows, literal width |
| B columns | longest path from the inputs; dummies for wires that skip columns |
| C order | reading order, then barycentre sweeps and transposes (fewest crossings) |
| D y | per column, least total wire `|dy|` without overlap; straighten |
| E lanes | per gutter (§5); untangle |
| F x, geometry | columns from widths and lane counts; polylines, junctions, crossing gaps |

### 4.2 Pass A: step boxes

Inputs on the left half, outputs on the right half (the old canvas), side by side: input i and output o share row height, `RowY(r)` from the
top. Header: step name, then the type dimmed if different. An input row holds its name and, when connected, its source chip (§6). Outputs:
the first always, the others when used; a vector output's components (`x/y/z/w`) are rows of their own under it only when shown (§6.5), else a connection from one leaves from the output's row and shows as `UV.x` on the chip.
Width = max(header, widest input + `MID_GAP` + widest output) + padding; height = `HEADER_H + max(inputs, outputs) * ROW_H + pad`. The
literal of an unconnected input with a parameter is drawn outside, left of its pin (label, widget, a link to the pin; gone once connected);
its width is the step's `LitW`.

### 4.3 Pass B: columns by depth

- Column = longest path from a step with no connected input: inputs, constants, samplers sit in column 0. The order invariant makes every
  edge point forward, so one pass in order computes it. The OUTPUT section's steps go to the last column (one past the deepest other step).
- Every connection therefore goes right (`column(to) > column(from)`): there are no same-column or leftward routes, so outputs stay on the
  right and inputs on the left (the owner's "move a leftward output to the bottom" rule is never needed).
- A wire that skips columns gets a dummy (a slot of height 0) in each column it crosses, one per (source output, column): all wires of one
  output share it. Dummies are ordered and placed like steps, so a long wire passes between steps, never behind one.
- An unwired step goes to the bottom of column 0: adding one moves nothing else.

### 4.4 Pass C: order in a column

Start from the reading order (sections, then steps; a dummy right after its source). Sweep: sort each column by the mean position of its
neighbours in the previous column (left to right) or the next (right to left), ports splitting a step by row; keep the order with fewest
crossings over 8 sweeps. Then transpose: swap two neighbours while that lowers the crossings of the column's two gutters. Ties keep the
reading order, so small edits keep the picture.

### 4.5 Pass D: y

Per column, items in order, gaps `NODE_GAP` between steps, `DUMMY_GAP` between a step and a passing wire, `DUMMY_SEP` between wires. Each
item wants its neighbours' port y minus its own port offset; the column takes the least total `|dy|` without overlap (isotonic L1 regression,
pool adjacent violators; a pool takes the end of its median interval nearer where it is). Alternating sweeps over the columns, then
straighten: an item moves to one of its wants when that fits and adds no length but makes one more wire straight. Steps are not stacked from
the top: each sits level with what it connects to.

### 4.6 Pass F: x

Columns left to right: a literal band (widest `LitW`), the steps, the gutter (`max(COLUMN_MARGIN, 2*ICON_CLEARANCE + (lanes-1)*LANE_GAP)`).
A step with a connected input starts at the band (inputs line up); one without ends at the column's right edge (its wires leave from there).

## 5. Routing (`xmaterial_stack_routes.h`)

### 5.1 Vocabulary

- **Gutter**: the space between columns g and g+1; vertical runs live only here.
- **Trunk**: all runs of one source **output** in one gutter: one vertical lane spanning its runs. A sub-element is its own output (`UV.x`
  and `UV.y` are two trunks). Different outputs never merge.
- **Lane**: an x in a gutter; lane 0 leftmost.

### 5.2 Route shape

A route leaves the producer's output to the right, and in each gutter it crosses turns once onto its trunk's lane, runs to the y of the next
dummy (or the consumer's input), and goes on right; through a column it passes at its dummy's y. Where the y already matches, the run is
straight and takes no lane: when nothing is in the way a route goes nearly directly, with at most two bends per gutter; it detours only
where Pass D had to move a dummy around a step. Horizontal runs lie in rows, dummy slots and gutters, so nothing crosses a step or a literal
by construction (the self-check asserts it).

### 5.3 Lanes

Per gutter: trunks with a vertical run are ordered left to right, each inserted where it adds the fewest crossings, then neighbours swapped
while that helps. Two trunks cross only when one's runs to the right pass the other's vertical or the other's runs from the left pass its
vertical, so the order of a pair alone decides its crossings: nested routes nest (a source farther from its turn takes the outer lane) and
never cross when they need not. Runs of two outputs on one y would merge: that order costs heavily. Each trunk then takes one lane right of
the overlapping trunks before it (the order of non-overlapping trunks changes no crossing).

Untangle: when two outputs' runs meet on one y in either order (two aligned steps swapping rows, or a straight run), the step or dummy at
the right end moves `MIN_SEP + 2` down (up if there is no room; else down, pushing the column below). Bounded; anything left is reported as
merged runs.

### 5.4 Drawing junctions and crossings

- A dot where a branch leaves a trunk that continues, or crosses its own trunk; a plain corner at a trunk's ends. Dots only ever join one
  output with itself.
- Crossing (two outputs): the vertical continuous, the horizontal broken 4 px each side, at a right angle. Never a dot at a crossing.

### 5.5 Stability

No lane, column or offset memory: every layout is a full, deterministic pack (`RepackRoutes` has no effect and stays for scripts;
`LayoutStats -Width` is ignored). Small edits keep the picture because ranks are local, ties follow the reading order and an unwired new step
waits at the bottom of column 0.

### 5.6 Emphasis

- Normal: every route drawn in its type colour at alpha 0.55, 1.5 px.
- Selected connection: its path from the source at full alpha, 2.5 px (nothing moves); the other branches of that output at 0.75.
- Selected step: its input and consumer routes lit.
- Trace upstream/downstream (`U` / `D`): transitive closure lit, the rest dimmed to 0.15.
- Collapsed section (§7.6): internal routes are not laid out; boundary routes end at the box rows.

## 6. Source chips

Every connected input row shows, after the input's name, a flat chip with the source's name: `StepName` for a single-output step,
`StepName.Output` for multi-output (`Wetness.Roughness`), `StepName.x` for a sub-element, then, for a vector input, one letter per component it receives, never shorthand (§6.5: `Vec4.xyzw`, `Vec2.xxxx`, `Albedo.xyz`, `Albedo.zyx`; a float input shows its single component, `Vec2.x`). Chips never change with zoom or distance. An
unconnected input shows no chip: its literal sits outside the step (§4.2).

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

Built (task 9): the chip is a flat box after the input's name; a hovered chip lights its route at full alpha and outlines the source step;
the focused chip (= the selected connection) has a blue border and lights its route, and the (+) of §7.1 appears on it. A chip is only drawn
for a connection that really ends at that input and leaves from a pin of a node: a stale `ConnectionGUID` (old files can name a connection
between pins that no longer exist) shows the input's literal, as the old canvas does. Go to source / Find all uses' pick centre the step and
push the history; refusals and validation are checked before a command runs, so a refused gesture leaves no history.

### 6.5 Components (swizzle and downcast)

Taking part of a vector is done often (a texture's RGB into a colour, its alpha as a mask, one channel splatted), so it has three gestures and
is always visible on the chip. How others do it: Unreal truncates a float4 into a float3 implicitly and has a ComponentMask node and fixed
RGB/R/G/B/A outputs on Texture Sample; Unity truncates and pads, with Split/Swizzle/Combine nodes; Godot truncates and puts an expand arrow on
vector outputs that reveals x/y/z/w ports; Blender has Separate/Combine nodes. Here (no new node: the components are a property of the input):

- **Truncation**: a longer vector feeds a shorter vector input, taking its first components; the chip shows it (`Albedo.xyz`; a whole vec4 into a vec4 reads `Vec4.xyzw`, a component splatted `Vec2.xxxx`; colours use `rgba`). Widening is
  refused (a component splats; a swizzle as long as the input widens explicitly: `.xyxy`). A vector never feeds a float implicitly (which
  component is a choice). SColor stays guarded (no components).
- **Component pins**: the arrow left of a vector output's name shows its components as pins on rows of their own (`.x .y .z .w`, `.r .g .b .a`
  for a Color; view state, `stack_view::m_Open`); while one of them is connected they stay shown, so a component's route always leaves from its
  own pin. While an input is dragged over a vector output whose components are hidden, they fan out beside its pin (an overlay: the layout does
  not move): release on one to connect it.
- **The picker**: a chip's component suffix (`.xyzw`, `.xxxx`, `.a`, underlined), chip menu Components..., or `.` on a focused chip; and a drop the types
  refuse but components can fix (a Vec4 onto a float input, a Vec2 onto a vec4) opens it at the drop. It lists all of it (or its first
  components), the other runs as long as the input (`.yzw`), each component (splatted into a vector input: `.xxxx`), and a field to type any swizzle
  (`bgr`, `xxyy`, Enter; focused when it opens, so a pick is letters + Enter).

One form for each meaning (`PickComponents`, `xmaterial_graph_editing.h`), so both canvases and the compiler agree: one component (or one
repeated) is the output's sub-element connection (what the old canvas's sub-element pins make); the first components in order are no swizzle
(truncation or exact); anything else is `connection::m_Swizzle` (2 to 4 letters, `xyzw`, as long as the input), compiled as `.zyx` after the
source. A vector input always spells every component out, in the chip and in the GLSL, inside a constructor of the input's type: `vec4(Vec2.xxxx) + vec4(Vec4.xyzw)`, `vec3(Albedo.xyz)` (only where a vector input reads another step's output, not literals or parameter reads; a float input keeps the plain expression). Commands: `Connect -Swizzle`, `SetInputSwizzle -Input -Swizzle <letters>|all` (the connection keeps its id); `InsertOnConnection`
keeps the swizzle on the new step's input when it fits, `DuplicateStep` copies it. The file stores `Swizzle` only on connections that have one
(like `ParamID`): files without swizzles stay byte-identical and older files read as before, so no version bump was needed.

Clicks and keys (left drag = 1 drag; old = the Material Graph canvas before this, which always shows sub-element pins; new stack canvas):

| Gesture | Old canvas | Stack canvas before | Stack canvas now |
|---|---|---|---|
| (a) Texture RGB into a vec3 input | right click, pick `Vec4 To Vec3`, 2 drags (4) | drag the input into empty space, type `to vec3`, Enter, then drag the Texture onto the new step (2 drags + ~8 keys) | 1 drag (truncation) |
| (b) Texture alpha as a float mask | 1 drag (from the `w` sub-pin) | drag the input into empty space, pick `Texture.w` in the search (1 drag + 1 click) | 1 drag (fan), or 1 drag + `a` Enter (picker), or arrow click + 1 drag |
| (c) Vec2.x splat into a vec4 input | 1 drag (from the `x` sub-pin) | as (b) (1 drag + 1 click) | 1 drag (fan) |
| (c') Vec2.x into the 4 inputs of a Vec4 | right click, pick Vec4, 5 drags | 4 x (1 drag + 1 click) + adding the Vec4 | not needed: (c) |
| a reorder (`.bgr`) | not possible without nodes | not possible | 1 click on the suffix + 3 letters + Enter |

## 7. Interaction contract

Every gesture = exactly one undo step: one command, or `xeditor::RunGroup` (`dependencies/xeditor/include/xeditor/commands.h:72`). View
gestures issue no command. The canvas issues GUIDs only; symbols are for people and AI.

### 7.1 Insert affordances (task 10; replaces the E27 (+) slots)

E27's slots between steps and at column edges (`DrawInsertMarker`, `NodeOS_UI_Canvas.h:1367-1481`) do not fit the depth layout: a column
holds steps of several sections in the order the layout picks, so the gap between two drawn steps names no place in the reading order. A new
step's place follows from what it binds to instead:
- **(+) on the selected route** (click a route; the (+) sits at the middle of its longest segment), or route menu (right click a route)
  "Insert step on this connection…": search of the types whose input takes the source and whose output fits the consumer; several
  (input, output) pairs => port chooser -> `InsertOnConnection`. Exactly that connection goes through the new step, placed right before the
  consumer; other uses of the source are untouched. The route menu also has Go to source, Go to consumer, Disconnect.
- **Drag from a pin** (the drag draws a wire to the mouse): onto a pin of the other side -> `Connect`; into empty space -> search of what
  fits: from an output, the types with a fitting input; from an input, the existing outputs (sub-elements too, `UV.x`) that sit before the
  consumer, then the types with a fitting output. Hovering a type shows a ghost of the new step at the drop point with its binding drawn;
  several fitting ports => port chooser (hovering a port moves the ghost's binding). A new step goes right after the step it reads
  (`CreateNode -After`) or right before the step it feeds (`-Before`); an output step goes to the output stack. Commit = group {`CreateNode
  -Name`, `Connect`}.
- **Background menu** (right click empty space) "Add step…": search of every type; **no wiring** -> `CreateNode` at the default slot (end
  of the last user section; unwired, so at the bottom of column 0, where it moves nothing).
- "New section here" is not offered on the canvas while sections are not drawn (§4); `CreateSection` / `PlaceSteps` from the console.

Contract: nothing is wired until the person picks a source or a type and, when several ports fit, a port. Every pick is one undo step.

### 7.2 Gestures

| Gesture | Commands (one undo step) | Validation |
|---|---|---|
| background menu -> Add step | `CreateNode -Prefab -Node` (default slot) | none needed (no wiring) |
| drag from an output pin into empty space | search filtered to prefabs with a compatible input; ghost preview of the new step with the binding pre-selected; several fitting inputs => chooser; commit = group {`CreateNode -After <producer>`, `Connect`} | none needed (a new step after its producer) |
| drag from an output pin, drop on an input | `Connect` | type, cycle, order (§7.3 if violated) |
| drag from an input pin into empty space / chip "Replace source" | search: existing compatible outputs before the consumer + prefabs; a new step goes right before the consumer: group {`CreateNode`, `Connect`} | type, order (only sources before the consumer are offered) |
| (+) on the selected route / route menu "Insert step on this connection…" | search filtered by in-type = source type, out-type = consumer type; chooser if several ports fit; placed right before the consumer: `InsertOnConnection` | types |
| drag a step header up or down its column | `PlaceSteps -Nodes -After\|-Before <step> -Resolve dependents` (§7.3) | none needed (one column: no binding between its steps) |
| Ctrl+click steps, step menu Collapse | group {`PlaceSteps`, `CreateSection`, (`CreateSection`, `PlaceSteps`), `PlaceSteps`, `SetSectionCollapsed`} (§7.6) | refused when the steps cannot sit together without moving others |
| step menu Duplicate | `DuplicateStep` (copies every input binding; outputs unconnected; placed after the original) | none |
| Del on a step | `DeleteNode` (also leaves its section; consumers fall back to widgets) | OUTPUT steps cannot be deleted from the stack |
| edit a param widget | `SetNodeProperty` (existing, value-based) | xproperty |
| rename (F2 / double-click the header) | `RenameStep` | unique, non-empty |
| expand a box (double click, menu Expand) | `SetSectionCollapsed -Collapsed 0` | none |
| select, hover, trace, go to, back, zoom, pan, preview | none | — |

### 7.3 Moves (task 11, re-thought for depth columns)

The v1 move (drag a step onto a slot between section steps, a popup when the drop breaks the order) does not fit v2: columns come from
dependency depth, so a step can only ever sit in its own column, and the reading order decides only the layout's free choices (§4.4). What a
person can still change is the vertical order of the steps of a column, which the layout derives from the reading order where it may.
- **Drag a step's header up or down its column.** The slot under the mouse is a line across the column; the drop is `PlaceSteps -Nodes <step>
  -Before|-After <neighbour> -Resolve dependents` (one undo step; bindings untouched). Steps of one column never bind each other, so the only
  order a drop can break is the step moving above one of its producers; `dependents` brings those along (they sit in earlier columns, so
  nothing visible moves for them). The three-choice popup is therefore never needed on the canvas; `PlaceSteps -Resolve none|nearest` and
  `ValidatePlacement` stay for the console.
- **Preview.** While dragging, the canvas lays out the order the drop would make (`FromGraph` with the planned sections, the same pure
  layout) and draws every step that would move as a flat card with its name where it would go, the routes it would draw faintly, and a label
  (`Detail2 before Detail`). When nothing would move (the barycentre order wins: fewer crossings) the label says so and the drop issues
  nothing: no invisible history. Esc cancels the drag.
- Deterministic and memoryless as before: the picture follows the stored reading order only.
- Not built: dragging sections (not drawn, §4); dragging across columns (meaningless: the column is the depth).

### 7.4 Search popup

Opened by (+), pin drag, replace, insert-on-connection. Filters: port type (exact `type_guid`, the only compatibility the compiler knows
today, `IsPinCompatible` `xmaterial_graph_editing.h:66`), sub-elements of wider outputs (offered as `Name.x`), conversion prefabs whose input
matches (listed under "via conversion", e.g. `Vec4 To Vec2`), stage (OUTPUT: FRAGMENT only in v1). Existing sources are listed only if they
sit before the consumer. Text filter over prefab and step names. Nothing is wired until the user picks a port.

### 7.5 Replace a shared source

"Find all uses" -> "Replace all…" -> pick a new source (search) -> group {`Connect` per use}. The new source must be before the earliest use;
otherwise §7.3 options apply to the new source.

### 7.6 Collapse (task 11)

A collapsed group is a USER section with `m_bCollapsed` set: organisation data only, saved and undone like every section.
- **Gesture.** Click a step, Ctrl+click more, step menu **Collapse N steps** (hovering it outlines the steps it takes). The group is the
  selection plus every step on a path between two of them (else the box would feed itself). Exactly a section: that section collapses.
  Otherwise one undo step: the steps go together to a slot after every producer outside them and before every consumer outside them, so no
  other step moves (`PlaceSteps`); they become a new section `Group` (the rest of the section they leave becomes `Name (2)`), collapsed. When
  no such slot exists without moving other steps (a step reading the group sits before a step feeding it) the collapse is refused, naming both.
- **Box.** One step-like box in the layout (`FromGraph`): header `Group  2 steps` with a card behind it; an input row per distinct source
  outside feeding a step inside (its chip text), an output row per inside output used outside. Boundary routes attach to the rows; routes
  inside are not laid out. Its column follows from its boundary like any step's.
- **Expand.** Double click the box or its menu Expand: `SetSectionCollapsed -Collapsed 0`; the section stays. The layout keeps no memory, so
  expanding gives exactly the picture from before the collapse (`LayoutStats` identical).
- `SelectStep -Node <section guid>` selects a box (scripts).

### 7.7 Validation messages (shown as a tooltip at the drop; nothing in history)

`types differ (Vec3 -> Float); use a sub-element or a conversion` (when components can fix it the drop opens the picker instead, §6.5) · `would make a cycle through <step>` · `<producer> is after <consumer>` (opens
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
| `SetInputSwizzle -Input <pin> -Swizzle <letters>\|all`, `Connect ... -Swizzle <letters>` | the components an input takes (§6.5) |

### 9.3 New queries (no undo)

| Command | |
|---|---|
| `ListStack [-Section]` | sections in order; per step: guid, name, type, inputs as chips (`Base <- Wetness.Darken`), unconnected inputs with values |
| `Sources -Node` | each input: chip text, connection guid, source pin guid |
| `FindUses -Node [-Output <name>]` | every consumer input of that step (or one output) |
| `ValidatePlacement -Nodes -Section (-After\|-Before...)` | dry run: `ok`, or the violations and the order each `-Resolve` would produce |
| `LayoutStats` | columns, steps and passing wires per column, lanes per gutter, crossings, junctions, route length, segments crossing steps (must be 0), layout ms |
| `RouteInfo -Connection` | columns, the lane per gutter (or straight), junctions, crossings on that route |
| `RepackRoutes` | no effect since the depth layout (§5.5); kept for scripts |
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

background -> routes (dimmed) -> lit routes (on top of dimmed ones, below steps) -> steps (header, inputs half, outputs half, chips, literals
left of the step) -> slots -> drag ghost / previews -> popups. No section frames (§4).

Look (owner feedback 2026-10-21): the old canvas's flat, quiet style: no shadows or accent bars; header in the kind's colour (function grey, exposed olive, input green,
output purple), inputs half grey, outputs half black; pins in the type colour, filled when connected, ringed when not; literals as in the
old canvas (label, widget, a green dot and link to the pin).
Background (owner feedback 2026-10-22, "hard to see"): the old canvas's colours, the node editor's `Bg` (60,60,70,200) as it lands over the
window, (59,59,67), and its 32 px `Grid` (120,120,120,40) faded by zoom squared; one grid, no major lines.

In-node previews (2026-10-22): a step can show what it makes under its ports, in rows added at the bottom of its box. The layout reads
those rows (`stack_view::m_PreviewRows`, passed to `FromGraph`), so the step is taller, its column spaces around it and no route crosses it;
no port sits in those rows. One hook in the canvas, `PreviewOf(node)`, says the kind:
- texture: a Sampler 2D shows its texture's thumbnail, a Texture step fed directly by a Sampler 2D shows the same one (4 rows). The picture
  is the editor's thumbnail cache through `xresource_editor::g_ReferenceHost.m_Thumbnail` (what every resource reference shows), the type's
  picture until it is ready, "no texture" when none is assigned. Readiness never changes the layout.
- colour: a Vec3 / Vec4 constant whose inputs are all literals shows a swatch of them (1 row; the literals are linear, clamped to 0-1 and encoded to sRGB for display; alpha ignored).
- rendered (2026-10-22): any other step whose first output the material's colour can take (directly, a float splatted, or through a
  conversion prefab, the §10.4 rule) shows that output on a flat quad (4 rows), from UV 0..1, unlit as the output writes it. The session's
  `step_previews` (`xmaterial_step_previews.h`) makes them: a step on the screen asks for its picture each frame with a key, a hash of
  everything upstream of it (prefabs, literals, connections, swizzles) and the declared parameters, so an edit makes again only the steps
  downstream of it. One step at a time: the §10.4 copy (`preview_compile`, shared with P) is compiled (~230 ms in Debug on the example),
  drawn by the mesh preview's own drawing (the PLANE3D model, no background) into a 64 px target, read back and written into the step's cell
  of one 1024 px atlas page (256 pictures, 4 MB, a cell freed by a step gone or not on the screen when one is needed). The older picture
  shows until the newer is ready; "..." before the first; "no preview" when it failed (hover: why). Headless there is none: the layout is
  unchanged. `StepPreviews` lists them (current, stale, none, failed; times made). Test: `smoke/test_material_step_previews.py`.
`T` (the panel focused) or the background menu's Show previews in steps toggles them (view state, on by default); off, the steps are as before.

### 10.3 GLSL names

`PinToVarName` (`source/Compiler/xmaterial_compiler.cpp:111-114`, today `"N_" + guid`) uses `StepVarName(node)` from
`xmaterial_step_names.h`: the step name made an identifier (letters, digits, `_`; leading digit or GLSL keyword => prefix `s_`), plus the
output name for multi-output steps (`wetness_roughness`); empty => `N_<guid>` as today. Uniqueness is guaranteed by step-name uniqueness. The
editor's error mapping (`xmaterial_editor.h:95`, matches the decimal guid in log lines) maps identifiers back through the same function.

### 10.4 One selected intermediate preview (first milestone)

`PreviewStep` (or P on the selected step; P again or Esc returns) makes a temporary copy of the graph whose output colour input takes the
selected output, compiles it as a side resource and shows it in Mesh Preview with a banner `Previewing: Wetness.Roughness  -  Esc to return`
over the canvas and the mesh (plus `compiling...` or the compiler's first error).

Assumption checked (task 13): the library compiles per asset (`library_mgr::RequestCompile` takes a library resource) and the editor has no
in-process GLSL compiler (shaderc is linked only into the compiler, a release build). But the compiler takes any descriptor path, and the
pipeline already accepts descriptors under `Cache\Descriptors\<Type>\..` (generated ones, `compiler_base::setupPaths`). So the editor
writes the copy to `Cache\Descriptors\Material\XX\YY\<new guid>.desc\Descriptor.txt`, runs the plugin's compiler with the library's
command line (`compilation::RunCommandLine`, on a thread), loads the result by guid through `g_Mgr` (it reads
`Cache\Resources\Platforms\WINDOWS\Material\XX\YY\<guid>`) and removes the temporary paths when the preview ends. The asset, its
descriptor, its compiled resource and the library DB are never written (the library watches `Descriptors`, not `Cache\Descriptors`; it only
logs the new log folder as unknown). No compiler flag needed.
Conversion: a vec3 feeds the colour directly, a float splats (the compiler's rule), another type goes through a prefab with one input of
that type and an output of the colour's type (`Vec4 To Vec3`); none => `a Vec2 cannot show on the mesh`. Textures and samplers are not
previewable. Alpha stays as the material has it. A compile of the material while previewing compiles the preview again from the new graph.

## 11. Task tests

Run each by hand on the canvas and as a script (`source/Editors/LevelEditor/smoke/test_material_stack_canvas.py`, pytest, Debug `xLION.exe`, nothing saved); record the measures from `LayoutStats`/`RouteInfo`/history. Every test also checks:
segments crossing steps = 0, order invariant holds, one undo step per gesture, undo then redo restores the identical file text.

| # | Test | Setup -> action | Pass | Measures |
|---|---|---|---|---|
| 1 | One UV feeding many branches | `UV` (Vec2) feeds 8 inputs over 3 sections | one trunk per container UV passes; junction dots, no parallel UV lanes; all 8 chips read `UV` | trunks for UV per highway = 1; total crossings; lanes per gutter |
| 2 | Independent normal and color branches | sections Normal, Surface color with no links between | no route between them; the two roots share column 0 (depth columns, the same at any width) | columns, steps in column 0; cross-section routes = 0 |
| 3 | Multi-output step | an `Inputs` step (outputs UV, Color) stands in for `Wetness`, each output used twice | chips `Wetness.UV(2)` / `Wetness.Color(4)`; two trunks, never merged | trunks = distinct outputs used (2); `FindUses -Output` correct |
| 4 | Insert tint into one branch | `Albedo` feeds Base color and Emission; insert `Vec4 Multiply` on Albedo->Base color | only that connection goes through the new step; chooser asked which input; Emission untouched | gestures ≤ 4; undo steps = 1; other connection guid unchanged; other steps' order unchanged |
| 5 | Reorder in a column (v2) | two independent branches in column 0; drag the second's header above the first | while dragging, the steps that would move drawn where they go; the drop swaps the branches; a drop the layout cannot honour says so and adds no history; Esc adds none | undo steps = 1 (0 for no-change / Esc); bindings unchanged; crossings not raised |
| 6 | Replace a shared mask | `Mask` used by 3 steps; Find all uses -> Replace all -> `Mask2` | 3 chips read `Mask2`; `Mask` still exists, unused | gestures ≤ 5; undo steps = 1 |
| 7 | Collapse steps (v2) | Ctrl+click steps with external inputs and an output used outside; Collapse | one box `Group  N steps`, a row per external source and per output used outside; boundary routes end at it; expand restores the identical layout | undo steps = 1; `LayoutStats` before collapse = after expand; segments through steps = 0 |
| 8 | Change an instance value | edit a Float widget on an unconnected input | value changes, nothing moves | step rects diff = 0; lane diff = 0; undo steps = 1 |
| 9 | Invalid connection | Vec3 output onto Float input; producer below consumer; a cycle | type and cycle refused with the message, no history; order case (producer below consumer): `Connect` moves the consumer and its dependents after the producer, one undo step (§7.3's popup was not built) | history size unchanged for refusals |
| 11 | Texture RGB into a vec3 input (§6.5) | drag the Texture's output onto the colour input | connects, chip `Albedo.xyz` | drags 1; undo steps 1 |
| 12 | Texture alpha as a float mask | from the alpha input over the Texture's pin to the fanned `.w`; then by the drop picker; then by the arrow | chip `Albedo.w`, the output's component pins shown | fan 1 drag; picker 1 drag + 2 keys; arrow 1 click + 1 drag; undo steps 1 each |
| 13 | Vec2.x splat into a vec4 input | from the vec4 input over the Vec2's pin to the fanned `.x` | chip `UV2.xxxx` | drags 1; undo steps 1 |
| 14 | Swizzle from the chip | click the suffix of `Albedo.xyz`, type `bgr`, Enter; then `w`; `SetInputSwizzle all`; wrong lengths | `Albedo.zyx` (connection id kept), `Albedo.www`, back to `Albedo.xyz`, refusals change nothing | 1 click + 4 keys; undo steps 1 each |
| 10 | Add one node to a large material | 200 steps; background menu Add step, then one reorder drag | the unwired new step waits at the bottom of column 0, nothing else moves; the drag preview costs one layout per slot change | steps moved by the add = 0; layout ms (`[A]` budget 2 ms; ~8 ms at 200 steps in release, §4.4 ponytail) |

### Results (2026-10-22, Debug xLION.exe, `test_material_stack_canvas.py`: 10 passed in 170 s)

Every test also passed the common checks (segments through steps 0, order invariant, one undo step per gesture, Undo then Redo restores the same
`ListStack`; the file text itself has no command, so the stack listing is what is compared).

| # | Result | Measured |
|---|---|---|
| 1 | pass | UV trunks per highway 1; crossings 0; junctions 6; columns 6; gutter lanes 1/1/1/0/0 |
| 2 | pass | columns 4; 2 steps in column 0 (the roots side by side, the same at `-Width 500`/`1600`: the layout ignores the width); cross-section routes 0; crossings 0 |
| 3 | pass | trunks 2 (= distinct outputs used); `FindUses -Output`: 2 routes per output; crossings 1 |
| 4 | pass | gestures 4 (click route, (+), Enter, port); undo steps 1; Emission connection guid unchanged; other steps' order unchanged; crossings 0 |
| 5 | pass | undo steps 1 (0 for the no-change drop and for Esc); graph untouched while previewing; bindings unchanged; crossings 0 -> 0 |
| 6 | pass | gestures 4 + typing; undo steps 1; 3 chips read `Mask2`, `Mask` unused |
| 7 | pass | undo steps 1; `LayoutStats` after expand = before collapse (4 columns, 5 routes, 0 crossings); collapsed: 4 columns, 4 routes; segments through steps 0 |
| 8 | pass | step rects diff 0 of 5; lane diff 0 of 3; undo steps 1 (value set with `SetNodeProperty`, the command the widget issues) |
| 9 | pass (order case differs from the spec) | type and cycle refused, graph and history unchanged; producer below consumer: accepted, consumer and dependents moved, 1 undo step |
| 10 | pass, timing not met in Debug | 202 steps; add by `CreateNode`: sampled steps moved 0 of 21 (the whole view origin shifts 84 px right, relative places equal); layout 252 ms before the add, 243 ms after, 264 ms after a reorder (Debug build: the 2 ms / ~8 ms release budget is not comparable and was not re-measured in Release); drag preview at 200 steps not driven (the step is off screen) |

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
   Status: done. Test 6 by mouse: chip menu -> Find all uses -> Replace all… -> type `Mask2`, Enter (4 gestures + typing, 1 undo step).
10. **(+) slots, search, pin drag** — `xmaterial_stack_canvas.h`. Slots incl. column edges, slot menu, search popup with filters, ghost
    preview, port chooser, insert-on-connection from a route menu. Done: tests 4 and 9 by mouse; no gesture wires anything without a pick.
    Status: done, with the affordances of §7.1 instead of slots (depth columns leave no reading-order gap to put a slot in). Test 4 by mouse:
    click the route, its (+), pick `Vec4 Multiply`, pick `A(4)` (4 gestures, 1 undo step, the Emission connection keeps its guid). Test 9:
    type and cycle drops refused with the message, history unchanged; the order case is resolved by `Connect` (dependents move) until the
    §7.3 popup lands with task 11.
11. **Moves and collapse** — `xmaterial_stack_canvas.h`. Step and section drag onto slots, the §7.3 popup with previews, collapsed section
    box with boundary rows. Done: tests 5, 7, 10.
    Status: done, re-thought for depth columns (§7.3, §7.6): reorder within a column by dragging the header, previewed, issuing nothing when
    nothing would move; Ctrl+click + Collapse into a new collapsed section (a box with boundary rows), double click to expand. Tests 5 and 7
    by mouse (smoke script, nothing saved: reorder 1 undo step, Esc and no-change drops 0; collapse 1 undo step, `LayoutStats` after expand
    = before collapse). Test 10's add was covered in task 10; its 200-step timing was not re-measured.
12. **Old canvas read-only** — `Editor/xmaterial_graph_canvas.h`. Positions from the stack layout, editing disabled, shared selection.
    Done: both panels show the same selection; no edit possible from the old one.
13. **One intermediate preview** — `xmaterial_stack_canvas.h`, `xmaterial_editor.h` (+ compiler only if §10.4's assumption fails).
    `PreviewStep`. Done: selecting a step and pressing P shows its output on the mesh; Esc returns to the material.
    Status: done (§10.4: the assumption holds with a descriptor under `Cache\Descriptors`; no compiler change). By mouse: click `Texture`,
    P, the mesh shows the texture under the banner; Esc, the material again; `PreviewStep -Node @Vec4_Multiply` from the console.
14. **Task test pass and docs** — run §11 by hand and as scripts, record measures in this file, update `documentation/editor.md` (stack
    panel, new commands). Done: all 10 pass; the owner decides when the old canvas goes.
    Status: done (2026-10-22): results in §11; `editor.md` describes the panel, its keys and every command.

15. **Components (swizzle and downcast)** — `xmaterial_stack_canvas.h`, `xmaterial_stack_routes.h` (component rows), `xmaterial_graph_editing.h`
    (truncation in `AreTypesCompatible`, `PickComponents`, `Connect -Swizzle`, `SetInputSwizzle`), `xmaterial_stack_commands.h` (chips),
    `Graph/xmaterial_graph.h` (`connection::m_Swizzle`), `Compiler/xmaterial_compiler.cpp` (`GetInputExpr`). §6.5.
    Status: done. Tests 11 to 14 by mouse; the 8 materials (7 examples + slice `5E1CE0A110000001`, copies) compile to the same SPIR-V hashes;
    a copy with a truncated and a swizzled connection compiles to `vec4(Vec4_Multiply3.xyz,1.0)` and `Texture.zyxw * Vec4`.

## 13. Deferred (not in the first milestone)

Expression steps, the structured text view, always-live thumbnails and typed on-demand previews, loop/switch regions (E27 owned markers and
scope depth), semantic zoom, subgraphs with interfaces, pin modifier chains (`[Texture.R] -> OneMinus -> Clamp01`; sketch: modifier nodes
owned by an input, like E27's owned markers, promotable to steps by clearing the owner), VERTEX stage in the output stack, saved lane hints, section slots on the canvas ("New section here", once sections are drawn again).

In-node rendered previews: done (§10.2). Open: a texture or shader file compiled again is not seen by the key (same guid); several compiles
at once if a large material fills slowly; each compile logs its temporary log folder as unknown to the library (as P does).

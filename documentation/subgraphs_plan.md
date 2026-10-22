# Material sub-graphs (plan)

A **sub-graph** is a resource (type `MaterialSubgraph`): a material graph with an interface, reusable from any material or other sub-graph.
A **group node** is a step in a graph that calls one. The user grows a library of them like any other resource. Owner decisions 2026-10-22:
a resource type, nesting included. The stack canvas's collapsed box (stack_canvas.md 7.6) stays as it is: a view fold, not a sub-graph.

## 1. What the others do (source in E:\source_code, checked 2026-10-22)

| Engine | Storage | Interface | Enter / exit | Compile |
|---|---|---|---|---|
| Unreal composite | inline (`UMaterialExpressionComposite`, pin bases of named reroutes) | crossing links at collapse | double click, same tab; breadcrumb/history back | inlined |
| Unreal Material Function | asset (`UMaterialFunction`), `FunctionInput`/`FunctionOutput` nodes | input/output nodes inside | double click opens the asset | inlined (`PushFunction`/`PopFunction`, no HLSL function) |
| Godot 4 | `VisualShaderGroup` resource, embedded or own file | `input_ports`/`output_ports` on the group, GroupInput/GroupOutput nodes inside | edit button pushes `group_edit_stack`, toolbar "up" pops; zoom/scroll per graph | GLSL function per group |
| Unity (no source) | `.shadersubgraph` asset | blackboard properties + SubGraphOutput | double click, new window | HLSL function |
| Blender (no source) | node-group datablock | interface on the tree, Group Input/Output nodes | Tab in/out, breadcrumb | flattened |
| O3DE | Material Canvas has none; GraphCanvas groups are frames | — | — | — |

Common pattern: explicit Group Input / Group Output nodes inside; Make Group turns each distinct crossing source into one pin, named after the
inner pin, made unique; recursion refused by a dependency walk.

## 2. Decisions (defaults; `[Q]` = for the owner)

1. **Storage.** A new plugin `plugins/xmaterial_subgraph.plugin` (one type per plugin). Its descriptor holds the same graph file as a
   material (`xmaterial_graph::graph::serialize`), so xtextfile, the stack, step names and every editing command are reused. No output
   section: its outputs are the Group Output node.
2. **Interface.** Two node kinds, at most one of each per sub-graph: **Group Input** (one output pin per sub-graph input) and **Group
   Output** (one input pin per sub-graph output). A pin is (stable pin guid, name, type). The pins are the node's own, saved per instance
   like every node's pins (the prefab has none). Commands: `AddGroupPin -Side In|Out -Name -Type`, `RemoveGroupPin`, `RenameGroupPin`,
   `RetypeGroupPin`, `MoveGroupPin`. An unconnected sub-graph input takes a default: the Group Input pin's literal (a float/vecN value).
3. **Group node (the call).** One prefab, `Subgraph`, with a resource param `Subgraph` (full guid, type filter `MaterialSubgraph`).
   Its pins mirror the referenced interface: the call's inputs are Group Input's pins, its outputs Group Output's pins. Call pin guid = mix(call node guid, interface pin guid), so renames and reorders keep every connection; a removed pin
   drops its connections (as "drop dangling connections" does today). Refreshed when the graph loads and when the sub-graph is saved
   (editor event). Header shows the sub-graph's name; step name defaults to it.
4. **Compile = flatten** (Unreal, Blender). Before `GenerateGLSL`, the material compiler replaces each call by the sub-graph's nodes:
   loads its descriptor, flattens it recursively, gives every inner node guid mix(call, inner) and step name `<Call>_<Inner>` (so GLSL
   names stay readable and unique), routes Group Input pins to the call's sources (or the default literal) and the call's consumers to
   what feeds Group Output. `GenerateGLSL` is unchanged. Each sub-graph's `Descriptor.txt` goes into `m_Dependencies.m_Assets`, so
   editing a sub-graph recompiles every material that uses it.
5. **Recursion.** Refused where it could appear: setting a call's `Subgraph` (the referenced one, transitively, must not reach the graph
   being edited) and at compile (a stack of guids being flattened; error names the loop).
6. **Sub-graph compiler.** Validates (types, cycles, every Group Output pin fed, no recursion) and writes the interface as its resource,
   so the asset browser shows errors on the sub-graph itself and thumbnails can come later.
7. **Library.** The stack canvas's Add step / pin-drag search lists every `MaterialSubgraph` in the open libraries under "Sub-graphs",
   filtered by the same port rules as prefabs.
8. **Enter / exit.** `[Q]` Double click a group node or select it and press Tab: the sub-graph opens in its own editor (the same
   editor code, registered for both types: its own document, undo and Save, since the sub-graph is shared by other materials), docked as
   a tab where the material is, with a breadcrumb bar `One Texture › Wetness › Noise`. Tab, Esc or a breadcrumb goes back to the parent's
   tab; each level keeps its zoom and scroll. Alternative: swap the graph inside the same canvas (Blender/Godot); rejected by default
   because one undo history would then mix two resources.
9. **Make Group** (Ctrl+click steps, menu "Make sub-graph…" or Ctrl+G): the cycle check of today's Collapse; a new `MaterialSubgraph`
   next to the material, named by the user; the steps move into it with a Group Input pin per distinct outside source and a Group Output
   pin per inside output used outside (names from the inner pin, uniqued); in the material they become one group node wired as they were.
   One undo step in the material; the new resource stays when that is undone (as Unreal's Collapse to Function).
10. **Ungroup** (group node menu "Expand here"): copies the sub-graph's nodes into the graph in place of the call (the flatten step of 4),
    one undo step. The resource is untouched.

## 3. Phases (each builds, keeps the editor usable, is committed alone)

1. **The plugin and its editor.** `xmaterial_subgraph.plugin` (type, descriptor, compiler that validates and writes the interface),
   CMake + host registration, the material editor opened for sub-graphs, Group Input / Group Output nodes and the pin commands. Done: a
   sub-graph is created in the asset browser, edited, saved and compiled.
2. **Group nodes and flattening.** The `Subgraph` prefab, pin mirroring, the library in search, the compiler's flatten + dependencies,
   recursion checks. Done: a material using a sub-graph (nested two deep) compiles to the same GLSL as the same steps written inline
   (C++ self-check, no editor), and recompiles when the sub-graph changes.
3. **Enter / exit, Make Group, Ungroup.** Breadcrumb, Tab/Esc, double click; Make Group from a selection; Expand here. Done: smoke
   script makes a group from steps, enters, edits, exits, and the material's GLSL is unchanged by make + ungroup.
4. **Docs and tests.** `documentation/editor.md` commands, results recorded here.

## 4. Status (2026-10-22: phases 1 to 4 built and committed; the first three limits below closed the same day, see 5)

Built as above, with 2.8 as the default (own tab plus breadcrumb). Where the code is: `Graph/xmaterial_subgraph.h` (validate, interface, the
group node's pins, `FlattenCalls`, `ExtractSubgraph`; no editor), `Editor/xmaterial_subgraph_commands.h` (the commands and enter/exit), the
canvas gestures in `Editor/xmaterial_stack_canvas.h`, the compiler's sub-graph mode and flatten in `Compiler/xmaterial_compiler.cpp`.

Checked: `Compiler/xmaterial_subgraph_test.py` (no editor: a material using a sub-graph, and one nested two deep, compile to byte-identical
SPIR-V as the inline steps; a loop is refused). In a live editor: the interface commands with undo/redo, a sub-graph compiling its interface,
a material with a group node compiling (`ZZTestAdder_Add = K + K`) with the sub-graph as dependency, a sub-graph edit compiling the material
again, Ungroup, Make sub-graph with undo/redo, Add step listing sub-graphs, double click into a group and the breadcrumb back. A crash found on
the way: an undo restore could fail to open its snapshot while the file watcher held it; `graph::serialize` now retries the open.

Limits, as built: the compiler reads sub-graphs from the project's own `Descriptors` (not other libraries); a group node in an open editor
takes a changed interface when the graph is read again (open, undo), not the moment the sub-graph is saved; an unconnected group input leaves
the steps that read it on their own literals; a sub-graph cannot declare material parameters; 2.2's default literal per interface pin and
`MoveGroupPin` are not built.

## 5. Closing the limits (2026-10-22)

1. **Other libraries.** The compiler looks for a sub-graph in its own project or library, then in every library that one names in
   `Project.config/Library.config.txt` (members and dependencies, transitively). The dependency is the sub-graph's descriptor by a relative
   path (`../Shared.lionlib/Descriptors/...`), so a stale material compiles again at the next start. Still open: the file watcher of the
   sub-graph's library does not tell the other libraries, so a material in another library than its sub-graph is not compiled again the
   moment the sub-graph is saved (it needs the pipeline to pass asset changes between libraries).
2. **Live interface.** Every open material or sub-graph editor listens for sub-graph compiles (any library); on the next frame its group nodes
   take the new interface, in memory, as a read does (`xmaterial_editor.h`, `OnAnyCompile`).
3. **Parameters.** A sub-graph declares material parameters like a material (ExposeParam and the rest work in its editor). Flattening adds
   its declarations to the material by id: every group node of that sub-graph reads the same parameter, and one the material declares itself
   stays as the material has it. Make sub-graph moves the declarations its steps read into the new sub-graph (the material keeps those its
   other steps still read); Ungroup gives them back. The sub-graph's compile refuses a read of an undeclared parameter, and its interface
   lists `param <type> <name>`.

Checked: `xmaterial_subgraph_test.py` also covers a sub-graph of another library (refused until the project names the library, then the
same GLSL as inline) and a sub-graph with a parameter called twice (one parameter, read twice; an undeclared read refused). The editor change
compiles; it was not run live (the renderer was mid-change in another thread).

Not in this plan: previews of a group node's outputs (the step preview path applies once it flattens), sub-graph thumbnails, a function-call
compile path (only if flattened shaders get too large).

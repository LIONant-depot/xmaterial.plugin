# The Material editor

Double-click a material in the asset browser (or `OpenResourceEditor -Asset <guid>`): the material is a graph of nodes on a canvas, previewed on a
mesh, with the shader the compiler made shown as text. Every edit of the graph is an undoable command, so an AI drives it from the command
console exactly as the canvas does.

```
source/Editor/xmaterial_editor.h            the editor (session): panels, commands, its registration
source/Editor/xmaterial_stack_canvas.h      the Material Stack panel: depth-column canvas, routes, chips, gestures, preview
source/Editor/xmaterial_stack_layout.h      its layout (pure); xmaterial_stack_routes.h: the router (pure)
source/Editor/xmaterial_graph_canvas.h      the old node canvas (imgui-node-editor), read-only
source/Editor/xmaterial_graph_editing.h     the graph commands and queries, symbol resolution, placement
source/Editor/xmaterial_stack_commands.h    the stack commands: PlaceSteps, RenameStep, sections
```

The generic half is `xeditor::document_editor` (`source/Tools/Editor/xeditor_document_editor.h`): the document is the graph file, edits are
snapshot- or value-based. A host includes `xmaterial_editor.h`, which registers the editor and compiles the material loader into the host.

## Panels

Material Stack (the stack canvas, below and documentation/stack_canvas.md), Material Graph (the old node canvas: positions from the stack layout, selection shared, no editing), Node Properties (the selected node's reflected properties), Mesh Preview, Shader (the compiler's output).

### The Material Stack panel

The graph is drawn as steps in columns by dependency depth (a step is in the column after its deepest producer; steps with no producer share column 0).
Routes (wires) run in the gutters between columns, one trunk per source output, with junction dots where they split and hops where they cross.
An input fed by a step shows a source chip (`Albedo`, `Wetness.Color`) instead of a wire stub; an unfed input shows its value widget. Nothing the
canvas does is saved in the layout: it follows the stored reading order, so every gesture below is one undo step (or none: view only) and the same
as the console command named in documentation/stack_canvas.md section 7.2.

| Gesture | Effect |
|---|---|
| click a step, a route or a chip | select it (the selection is shared with the Material Graph panel); a route's (+) appears where it was clicked |
| Ctrl+click steps | add to the selection (a group, for Collapse) |
| edit a widget on an unconnected input | `SetNodeProperty` |
| click the (+) of the selected route, or right click a route -> Insert step on this connection... | search of the types that fit the source and the consumer, a port chooser when several ports fit, then `InsertOnConnection`; other uses of the source are untouched. The route menu also has Go to source, Go to consumer, Disconnect |
| drag from a pin onto a pin of the other side | `Connect`; refusals (types, cycle) are shown as a tooltip and add no history; a producer below its consumer moves the consumer and its dependents after it |
| drag from a pin into empty space | search of what fits (from an output: types with a fitting input; from an input: existing outputs before the consumer, then types), a ghost of the new step shows at the drop; nothing is wired until a type (and port) is picked |
| right click empty canvas -> Add step... | search, `CreateNode` (unwired, at the end of the last section) |
| double click a chip, Enter on a selected route, chip menu Go to source | centre and select the source step (Alt+Left or the mouse back button returns) |
| chip menu | Go to source, Find all uses (every consumer of that output; pick one to go there; Replace all... picks a new source for all of them, one undo step), Replace source..., Disconnect |
| drag a step's header up or down its column | reorder (`PlaceSteps`); the layout the drop gives is previewed, a drop that changes nothing issues nothing, Esc cancels |
| right click a step | menu: Trace upstream / downstream, Preview on the mesh, Collapse N steps (hover outlines the steps it takes: the selection plus the steps between them) |
| double click a collapsed box, or its menu Expand | open the group (a collapsed group is a section with the collapsed flag; the picture after expanding equals the one before collapsing) |
| wheel / right drag / Home | zoom 0.5 to 1.5 under the cursor / pan / reset the view (a right click that does not move opens the menu of what is under it) |

Keys (the panel focused, no text being typed): `U` / `D` trace upstream / downstream from the selected step (again to stop), `P` preview the selected
step's output on the mesh, `T` shows or hides the previews in steps (a Sampler 2D's texture, and a Texture step's fed by one; a constant colour's
swatch; also in the background menu), `Esc` stops, in order, a drag, the preview, the trace, the selection; `Tab` / `Shift+Tab` move focus along the chips,
`Enter` goes to the source of the selected route, `Del` disconnects it, `Alt+Left` goes back. The preview and the trace are view state (never
saved, never in the history); the asset is never written by a preview.

The same view state is scriptable: `SelectStep`, `TraceRoute`, `PreviewStep`, `LayoutStats`, `RouteInfo` (below). The smoke tests
`source/Editors/LevelEditor/smoke/test_material_stack_canvas.py` drive the gestures above with the virtual mouse and check them with these commands.

## Commands

Run as `<resource name>\<Command>`. Nodes, pins and connections are named by guid (hex); text is written in quotes. Wherever a node or pin is
taken a symbol also works: `@StepName` (a step's name, case-insensitive), `@StepName.Pin` (an output, a sub-element `x` or `Output.x`, or an input;
`A` matches `A(4)`; `@Step` alone is the output of a single-output step). Sections take a guid or `@Name` (`@"Surface color"`). Symbols are resolved
when the command runs. New guids are always given by the caller. Every graph has a stack (documentation/stack_canvas.md): sections in order, each
with its ordered steps; a producer is always before its consumers, and every command below keeps that.

| Command | |
|---|---|
| `ListNodeTypes`, `ListNodes`, `NodeProperties -Node [-Filter]` | what can be created, what is in the graph (with the pins' guids, each step's name and section, then the sections in order), a node's properties |
| `CreateNode -Prefab <type hex> -Node <new guid> [-Section <s> [-After <node> \| -Before <node> \| -First 1 \| -Last 1]] [-Name]`, `DeleteNode -Node` | nodes (undoable); the pin guids of a new node are derived from its own, so undo and redo keep later commands valid. A new node is also a step: placed where asked, else at the end of the last user section (outputs: the output section), named `-Name` or its type's name made unique. `-X -Y` are accepted and ignored. `DeleteNode` also removes the step and refuses the last output node |
| `Connect -Output <pin> -Input <pin> -Connection <new guid>`, `Disconnect -Connection` | links (undoable). `Connect` accepts the same type, or a float (also a sub-element such as `Vec2.x`) into a vec2/vec3/vec4 input (a splat, compiled as `vecN(x)`), or `Color` to and from `Vector4` (both vec4); an `SColor` only into an `SColor` input (`SColor is sRGB-encoded; convert with 'SColor To Color'`), a `Color` never into one (`Color is linear; convert with 'Color To SColor'`), material_contract.md §3; other widths, float to int and vector to scalar need a conversion or sub-element and are refused (`types differ`). `Connect` refuses a cycle (`would make a cycle through <step>`); when the producer is after the consumer, the consumer and its dependents move to right after the producer, in the same undo step |
| `MoveNode -Node -X -Y [-BeforeX -BeforeY]` | the position on the old canvas (undoable); old canvas only, the stack canvas ignores positions |
| `PlaceSteps -Nodes <n,n,...> -Section <s> [-After <node> \| -Before <node> \| -First 1 \| -Last 1] [-Resolve none\|dependents\|nearest]` | move steps, keeping their bindings (undoable). `none` (default) fails naming the violations; `dependents` also moves what must follow (or precede) them; `nearest` takes the closest valid spot |
| `RenameStep -Node -Name` | a step's name: letters, digits and `_` (other characters become `_`), unique ignoring case (undoable) |
| `CreateSection -Section <new guid> -Name [-After <s> \| -Before <s>]`, `RenameSection -Section -Name`, `DeleteSection -Section [-MergeInto <s>]`, `SetSectionCollapsed -Section -Collapsed 0\|1` | sections (undoable). A new section is empty and never after the output section; the output section cannot be deleted; a section with steps is deleted only with `-MergeInto` (refused when that breaks the order) |
| `MoveSection -Section (-After <s> \| -Before <s>) [-Resolve ...]` | the section order (undoable); the output section stays last |
| `InsertOnConnection -Connection <c> -Prefab <t> -Node <new> -Input <pin> -Output <pin> [-Name]` | a new step on one connection (undoable): the source feeds `-Input`, `-Output` feeds the old consumer, which keeps its other bindings; placed right before the consumer (an output consumer: the end of the last user section). Refuses differing pin types (`types differ (A -> B); ...`). The two new connections get ids derived from `-Node` |
| `DuplicateStep -Node -NewNode <new> [-Name]` | copies a step with its values (parameter links too) and every input binding; its outputs feed nothing; placed right after the original (undoable). Connection ids derived from `-NewNode` |
| `ListStack [-Section]`, `Sources -Node`, `FindUses -Node [-Output]`, `ValidatePlacement -Nodes -Section (-After\|-Before\|-First\|-Last)` | the stack with each step's inputs as chips (`Base <- Wetness.Darken`) or values; the source of each input of a step; every input fed by a step or one output; `PlaceSteps` as a dry run: `ok`, or the violations and the order each `-Resolve` would give |
| `LayoutStats`, `RouteInfo -Connection`, `StackSelfTest` | the stack canvas layout (view state, not undoable): depth columns, route length, crossings, junctions, segments through steps (must be 0); how one connection is routed; the layout and router self-check. The layout is deterministic, so `RepackRoutes` and `-Width` are accepted but do nothing |
| `SelectStep [-Node <n>\|<collapsed section guid> \| -Connection <c>]`, `TraceRoute -Node -Direction up\|down\|off`, `PreviewStep [-Node <n> [-Output <name>]]` | view state of the Material Stack (not undoable): select (replies where it is on screen), trace, and show one step's output on the Mesh Preview instead of the material (no `-Node`: back to the material). The preview compiles a temporary copy of the graph whose output colour takes that output (a float splats; a Vec4 goes through `Vec4 To Vec3`) with the material compiler, its descriptor in `Cache\Descriptors` (removed when the preview ends): the asset is never written. A banner says what is shown, over the canvas and the mesh |
| `ExposeParam -Node -Param <input> [-Id <param>] [-Name] [-Type COLOR\|FLOAT2\|FLOAT3\|FLOAT4]`, `UnexposeParam -Node -Param` | an unconnected float or texture input reads a material parameter instead of its literal (undoable). A new parameter takes the input's pin guid as id, its literal as default; `-Id` (hex or `@Name`) attaches the input to an existing parameter of the same type. `-Type` exposes all the inputs of a Vec2/Vec3/Vec4 node together as one vector parameter (input i reads component i; a COLOR needs a Vec3, no alpha, or a Vec4). Unexpose gives the literal back (the default); the declaration stays. `NodeProperties` and the Node Properties panel show `param link: Input -> parameter 'Name'` |
| `SetParamInfo -Id [-Name -Group -UiMin -UiMax -ValidMin -ValidMax -HDR -Alpha -EntityField]`, `ListParams` | a parameter's metadata (undoable; ranges are 1 to 4 comma separated numbers); the parameters with their readers and the place in the block of the last compile |
| `SetParamDefault -Id -Value [-Policy Inherit\|PreserveLook]`, `RetypeParam -Id -Type [-Convert Splat\|Truncate]`, `PurgeUnusedParams` | the migration ops (undoable, the undo also puts the instance files back). PreserveLook writes the old default into the instances made directly from the material that did not set it; `-Convert` converts the defaults and the instances' values and rewrites the instance files; Purge deletes the declarations nothing reads (instances' values stay as orphans) |
| `SetNodeProperty -Node -Path -Value [-Before]` | a property of a node, e.g. `node/Params[G:0]/Float` (undoable) |
| `SetShaderFile -Node -File` | the shader text of a code node (undoable) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetPreviewMesh [-Model name]` | the mesh the material is shown on (no name lists them) |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |

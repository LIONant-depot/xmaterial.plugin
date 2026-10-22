# The Material editor

Double-click a material in the asset browser (or `OpenResourceEditor -Asset <guid>`): the material is a graph of nodes on a canvas, previewed on a
mesh, with the shader the compiler made shown as text. Every edit of the graph is an undoable command, so an AI drives it from the command
console exactly as the canvas does.

```
source/Editor/xmaterial_editor.h            the editor (session): panels, commands, its registration
source/Editor/xmaterial_graph_canvas.h      the node canvas (imgui-node-editor): drag, link, menus
source/Editor/xmaterial_graph_editing.h     the graph commands and queries, symbol resolution, placement
source/Editor/xmaterial_stack_commands.h    the stack commands: PlaceSteps, RenameStep, sections
```

The generic half is `xeditor::document_editor` (`source/Tools/Editor/xeditor_document_editor.h`): the document is the graph file, edits are
snapshot- or value-based. A host includes `xmaterial_editor.h`, which registers the editor and compiles the material loader into the host.

## Panels

Material Graph (the canvas), Node Properties (the selected node's reflected properties), Mesh Preview, Shader (the compiler's output).

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
| `Connect -Output <pin> -Input <pin> -Connection <new guid>`, `Disconnect -Connection` | links (undoable). `Connect` accepts the same type, or a float (also a sub-element such as `Vec2.x`) into a vec2/vec3/vec4 input (a splat, compiled as `vecN(x)`); other widths, float to int and vector to scalar need a conversion or sub-element and are refused (`types differ`). `Connect` refuses a cycle (`would make a cycle through <step>`); when the producer is after the consumer, the consumer and its dependents move to right after the producer, in the same undo step |
| `MoveNode -Node -X -Y [-BeforeX -BeforeY]` | the position on the old canvas (undoable); old canvas only, the stack canvas ignores positions |
| `PlaceSteps -Nodes <n,n,...> -Section <s> [-After <node> \| -Before <node> \| -First 1 \| -Last 1] [-Resolve none\|dependents\|nearest]` | move steps, keeping their bindings (undoable). `none` (default) fails naming the violations; `dependents` also moves what must follow (or precede) them; `nearest` takes the closest valid spot |
| `RenameStep -Node -Name` | a step's name: letters, digits and `_` (other characters become `_`), unique ignoring case (undoable) |
| `CreateSection -Section <new guid> -Name [-After <s> \| -Before <s>]`, `RenameSection -Section -Name`, `DeleteSection -Section [-MergeInto <s>]`, `SetSectionCollapsed -Section -Collapsed 0\|1` | sections (undoable). A new section is empty and never after the output section; the output section cannot be deleted; a section with steps is deleted only with `-MergeInto` (refused when that breaks the order) |
| `MoveSection -Section (-After <s> \| -Before <s>) [-Resolve ...]` | the section order (undoable); the output section stays last |
| `InsertOnConnection -Connection <c> -Prefab <t> -Node <new> -Input <pin> -Output <pin> [-Name]` | a new step on one connection (undoable): the source feeds `-Input`, `-Output` feeds the old consumer, which keeps its other bindings; placed right before the consumer (an output consumer: the end of the last user section). Refuses differing pin types (`types differ (A -> B); ...`). The two new connections get ids derived from `-Node` |
| `DuplicateStep -Node -NewNode <new> [-Name]` | copies a step with its values (parameter links too) and every input binding; its outputs feed nothing; placed right after the original (undoable). Connection ids derived from `-NewNode` |
| `ListStack [-Section]`, `Sources -Node`, `FindUses -Node [-Output]`, `ValidatePlacement -Nodes -Section (-After\|-Before\|-First\|-Last)` | the stack with each step's inputs as chips (`Base <- Wetness.Darken`) or values; the source of each input of a step; every input fed by a step or one output; `PlaceSteps` as a dry run: `ok`, or the violations and the order each `-Resolve` would give |
| `LayoutStats`, `RouteInfo -Connection`, `StackSelfTest` | the stack canvas layout (view state, not undoable): depth columns, route length, crossings, junctions, segments through steps (must be 0); how one connection is routed; the layout and router self-check. The layout is deterministic, so `RepackRoutes` and `-Width` are accepted but do nothing |
| `ExposeParam -Node -Param <input> [-Id <param>] [-Name]`, `UnexposeParam -Node -Param` | an unconnected float or texture input reads a material parameter instead of its literal (undoable). A new parameter takes the input's pin guid as id, its literal as default; `-Id` (hex or `@Name`) attaches the input to an existing parameter of the same type. Unexpose gives the literal back (the default); the declaration stays. `NodeProperties` and the Node Properties panel show `param link: Input -> parameter 'Name'` |
| `SetParamInfo -Id [-Name -Group -UiMin -UiMax -ValidMin -ValidMax -HDR -Alpha -EntityField]`, `ListParams` | a parameter's metadata (undoable; ranges are 1 to 4 comma separated numbers); the parameters with their readers and the place in the block of the last compile |
| `SetNodeProperty -Node -Path -Value [-Before]` | a property of a node, e.g. `node/Params[G:0]/Float` (undoable) |
| `SetShaderFile -Node -File` | the shader text of a code node (undoable) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetPreviewMesh [-Model name]` | the mesh the material is shown on (no name lists them) |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |

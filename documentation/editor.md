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
| `Connect -Output <pin> -Input <pin> -Connection <new guid>`, `Disconnect -Connection` | links (undoable). `Connect` refuses a cycle (`would make a cycle through <step>`); when the producer is after the consumer, the consumer and its dependents move to right after the producer, in the same undo step |
| `MoveNode -Node -X -Y [-BeforeX -BeforeY]` | the position on the old canvas (undoable); old canvas only, the stack canvas ignores positions |
| `PlaceSteps -Nodes <n,n,...> -Section <s> [-After <node> \| -Before <node> \| -First 1 \| -Last 1] [-Resolve none\|dependents\|nearest]` | move steps, keeping their bindings (undoable). `none` (default) fails naming the violations; `dependents` also moves what must follow (or precede) them; `nearest` takes the closest valid spot |
| `RenameStep -Node -Name` | a step's name: letters, digits and `_` (other characters become `_`), unique ignoring case (undoable) |
| `CreateSection -Section <new guid> -Name [-After <s> \| -Before <s>]`, `RenameSection -Section -Name`, `DeleteSection -Section [-MergeInto <s>]`, `SetSectionCollapsed -Section -Collapsed 0\|1` | sections (undoable). A new section is empty and never after the output section; the output section cannot be deleted; a section with steps is deleted only with `-MergeInto` (refused when that breaks the order) |
| `MoveSection -Section (-After <s> \| -Before <s>) [-Resolve ...]` | the section order (undoable); the output section stays last |
| `SetNodeProperty -Node -Path -Value [-Before]` | a property of a node, e.g. `node/Params[G:0]/Float` (undoable) |
| `SetShaderFile -Node -File` | the shader text of a code node (undoable) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetPreviewMesh [-Model name]` | the mesh the material is shown on (no name lists them) |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |

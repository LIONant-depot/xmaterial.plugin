# The Material editor

Double-click a material in the asset browser (or `OpenResourceEditor -Asset <guid>`): the material is a graph of nodes on a canvas, previewed on a
mesh, with the shader the compiler made shown as text. Every edit of the graph is an undoable command, so an AI drives it from the command
console exactly as the canvas does.

```
source/Editor/xmaterial_editor.h            the editor (session): panels, commands, its registration
source/Editor/xmaterial_graph_canvas.h      the node canvas (imgui-node-editor): drag, link, menus
source/Editor/xmaterial_graph_editing.h     the graph commands and queries
```

The generic half is `xeditor::document_editor` (`source/Tools/Editor/xeditor_document_editor.h`): the document is the graph file, edits are
snapshot- or value-based. A host includes `xmaterial_editor.h`, which registers the editor and compiles the material loader into the host.

## Panels

Material Graph (the canvas), Node Properties (the selected node's reflected properties), Mesh Preview, Shader (the compiler's output).

## Commands

Run as `<resource name>\<Command>`. Nodes, pins and connections are named by guid (hex); text is base64.

| Command | |
|---|---|
| `ListNodeTypes`, `ListNodes`, `NodeProperties -Node [-Filter]` | what can be created, what is in the graph (with the pins' guids), a node's properties |
| `CreateNode -Prefab <type hex> -Node <new guid> [-X -Y]`, `DeleteNode -Node` | nodes (undoable); the pin guids of a new node are derived from its own, so undo and redo keep later commands valid |
| `Connect -Output <pin> -Input <pin> -Connection <new guid>`, `Disconnect -Connection` | links (undoable) |
| `MoveNode -Node -X -Y [-BeforeX -BeforeY]` | position (undoable) |
| `SetNodeProperty -Node -Path -Value [-Before]` | a property of a node, e.g. `node/Params[G:0]/Float` (undoable) |
| `SetShaderFile -Node -File` | the shader text of a code node (undoable) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetPreviewMesh [-Model name]` | the mesh the material is shown on (no name lists them) |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |

# Material render settings (plan)

Status: **built** (2026-10-23), with the open points decided as defaults: masked surfaces are solid for the GI, one masked shadow
pipeline per material, translucent lit like opaque, no per-entity sort priority. The pipeline settings belong to the material, not the
instance (each combination is its own Vulkan pipeline); they are edited on the output node but stored once on the material. A masked
output writes alpha 1 where it is kept (the alpha is the tone map's coverage), as opaque does.

`file:line` points at the code as it was when this was written.

## 1. Where things stand

- The material carries one render flag, `m_bAlpha` (`source/xmaterial_data_file.h:24`), and the compiler always writes `false`
  (`Compiler/xmaterial_compiler.cpp`, `m_Flags.m_bAlpha = false`). The descriptor's `bAlpha` (`source/xmaterial_rsc_descriptor.h:12`) is
  not wired to it.
- xLIONRender: `CreateMaterial(..., bool bAlpha, ...)` (`xlionrender_instance.cpp:807`) stores it, and only the GI reads it (it sees
  through those materials, `plans/forward/xlionrender_plan_forward.cpp:193`). The material pipelines (`getMaterialInstance`, :850) use
  xGPU's defaults: back-face cull, depth test and write on, no blend.
- The shadow pipelines are the renderer's own, depth only, without the material's shader (`xlionrender_instance.cpp:496`): no material
  can cut holes in its shadow today.
- A commented placeholder for these settings sits in `Graph/xmaterial_graph.h:62` (blend, sorting, culling order, wire-frame).
- xGPU has everything needed (`xGPU/source/xgpu_pipeline.h`): `primitive` (raster FILL/WIRELINE, front face, cull NONE/FRONT/BACK),
  `blend` (enable, factors, ops), `depth_stencil` (test, write, compare, bias).

## 2. The settings

Named settings, the way Unreal, Unity and Godot expose them, never raw Vulkan state. Each maps to xGPU fields in one place (the renderer).

| Setting | Values (default first) | Pipeline (xGPU) | Shader |
|---|---|---|---|
| Blend Mode | Opaque, Masked, Translucent, Additive | blend off / off / src-alpha, one-minus-src-alpha / one, one | Masked: `discard` below the cutoff |
| Alpha Cutoff | 0.5 (Masked only) | none | a material parameter (instances may change it, no new pipeline) |
| Double Sided | off | cull BACK / NONE | back faces: the normal flipped (`gl_FrontFacing`) |
| Back Face Normal | Flip, None (Double Sided only) | none | Flip: as HDRP's "Flip"; None: foliage cards lit as the front |
| Depth Write | Auto (Opaque, Masked: on; Translucent, Additive: off), On, Off | `m_bDepthWriteEnable` | none |
| Depth Test | On, Off | `m_bDepthTestEnable` | none |
| Cast Shadows | on | the shadow pass draws it or not | Masked: the shadow pass runs the cutoff too |
| Sort Priority | 0 (Translucent, Additive only) | none: draw order, back to front within one priority | none |
| Wireframe | off (debug) | raster WIRELINE | none |

`m_bAlpha` goes: "the GI sees through it" becomes Blend Mode != Opaque (Masked: [open point 1]).

## 3. Storage

- **Graph file (the descriptor):** one `[ RenderSettings ]` record after `[ Info ]`, written by `graph::serialize`
  (`Graph/xmaterial_graph.cpp:283`); the file version goes 3 -> 4, v3 files load with the defaults (they render as today: Opaque,
  single sided). A `render_settings` struct in `Graph/xmaterial_graph.h` replaces the commented placeholder, with xproperty for text I/O
  and the editor.
- **Compiled material (`xmaterial::data_file`):** the settings packed next to `m_Flags` (a few bytes; `m_bAlpha` replaced by the blend
  mode), the serializer version bumped. Alpha Cutoff is not here: it is a declared parameter in the block, like Roughness.
- **Instances:** nothing. They keep only parameters and textures; an instance override of a mode (Unreal's "Material Property
  Overrides") would be a new pipeline per instance and is left out until it is needed.

## 4. Compiler

- Writes the settings into the data file.
- Masked: emits the cutoff `discard` after the output's alpha is known (`Output: PBR` and `Output: Target Color` read their A input) and
  declares `Alpha Cutoff` as a material parameter.
- Double Sided with Flip (superseded by part three: the flip is always compiled in, None defines `XMAT_BACK_FACE_AS_FRONT`): defines `XMAT_DOUBLE_SIDED` so `PBRLighting` (xLIONRender `mb_standard_pbr.frag`) flips N when
  `!gl_FrontFacing` (one guard there).
- `Output: Shader File` materials get the same settings; their shader handles masking and flipping itself.

## 5. Renderer (xLIONRender)

- `CreateMaterial` takes a small `render_state` instead of `bool bAlpha`; `getMaterialInstance` sets `m_Primitive`, `m_Blend` and
  `m_DepthStencil` from it. Pipelines are already per material, so this adds no pipeline cache.
- The forward plan draws Opaque and Masked first, then Translucent and Additive sorted back to front by Sort Priority, then distance.
  [UNVERIFIED: how the forward plan orders draws today; read before building.]
- Shadows: Cast Shadows off skips the draw; Masked casters need a shadow pipeline that runs the material's cutoff [open point 2].
- The three `CreateMaterial` callers (`xlionrender_scene_renderer.h:139`, `xgeom_static_editor_preview.h:291`,
  `xgeom_skin_editor_preview.h:325`) pass the material's state.
- The editors' LDR previews (`xeditor_mesh_preview.h`) build their pipeline the same way, from the same state.

## 6. Editor

- Selecting an output node shows its inputs and then a **Material** group with these settings in Node Properties. They edit the one
  material-level record, whichever output node (or the Shader File node) is selected.
- Edits are undoable commands, like every other graph edit, e.g. `SetRenderSetting -Name "Blend Mode" -Value Masked`, and
  `ListRenderSettings`. Settings that do not apply (Alpha Cutoff when not Masked) are greyed out, not hidden.
- The Mesh Preview shows the change after the compile, as today.

## 7. Open points

1. Masked and the GI: is a masked surface seen through by the GI (as translucent), or solid? Solid is cheaper and right for most foliage.
2. Masked shadows: a per-material shadow pipeline (the material's shader in a depth-only pass) or a shared one reading the albedo
   texture's alpha? The first is exact, the second fewer pipelines.
3. Translucent lighting: lit like opaque (one layer, no depth write) to start; refraction or per-pixel sorting later.
4. Should Sort Priority also be on the entity (Unity's render queue offset), for two translucent objects using one material?

## 8. Steps (each leaves the build green, existing materials unchanged)

1. Settings struct, graph file v4, data file fields (defaults = today). Done: old materials load, compile, render as before.
2. Editor: the Material group on output nodes and the commands.
3. Renderer: `render_state` in `CreateMaterial`/`getMaterialInstance`; Double Sided and Wireframe first (pipeline only).
4. Masked: cutoff parameter, `discard`, GI rule, shadows.
5. Translucent and Additive: blend, depth write, sorted draws.
6. Tests: a compiler check per mode (like `xmaterial_pbr_output_test.py`) and a render test per mode in xLIONRender's tests.

# Part two: constants × textures in the default PBR material (plan)

Status: **built** (2026-10-23). Default PBR Material and PBR Alpha Test (Masked) are graphs on `Output: PBR`; their texture parameters
keep the ids of the old shader file's slots (#6E normal, #78 albedo, #82 ORME), so the instances made before keep their textures. As built:
NormalStrength defaults to 1.5 (what mb_material_pbr.glsl used), AO is a `Float Mix` step (mix(1, ORME.r, AOStrength)), the colours are
vec3 (`Vec3 Multiply`): Base Color = Albedo.rgb × BaseColorFactor.rgb × the vertex colour (white without a COLOR0 stream), and the
emission multiplies that base colour (an emitter without a color map has its colour in BaseColorFactor). The alpha test's cut is
Albedo.a × BaseColorFactor.a × the vertex colour's alpha. The normal map step is `Normal Map (BC5: xy in GR)`; `(BC3: xy in AG)` and `(RGB: BC7, uncompressed)`
decode the other ways the texture compiler stores a normal (the default material, like the old shader, expects BC5). The importer writes the
factors only into instances it makes; an existing instance keeps its constant textures (they are the user's).

The ORME packing stays (every channel is needed). The question is what a material does when
a model has no map for a channel, so that constants become instance parameters instead of textures made per material.

## What the competition does (read in their source, E:\source_code)

| Engine | Rule | Where |
|---|---|---|
| Filament (glTF) | `baseColor = baseColorFactor`, then `*= texture` when there is one; same for roughness/metallic/emissive; `ao = 1 + aoStrength * (occlusion - 1)`; `normal.xy *= normalScale` | `filament/libs/gltfio/src/JitShaderProvider.cpp:126-230` |
| Bevy | factor × texture: `base_color *= texture`, `perceptual_roughness *= metallic_roughness.g`, `metallic *= metallic_roughness.b`, `emissive.rgb * texture` | `bevy/crates/bevy_pbr/src/render/pbr_fragment.wesl:101-365` |
| Godot StandardMaterial3D | `ALBEDO = albedo * albedo_tex`; `METALLIC = metallic_tex * metallic`, `ROUGHNESS = roughness_tex * roughness` (missing maps default white: `hint_default_white`); emission `(color * tex) * energy` (or add); **ORM mode: `ROUGHNESS = orm_tex.g`, `METALLIC = orm_tex.b`, `AO = orm_tex.r` with no factors** | `godot/scene/resources/material.cpp:1003, 1660-1725, 1770-1778, 1942` |
| O3DE StandardPBR | base colour: texture blended with the factor colour (Multiply by default); **metallic and roughness: the texture replaces the factor** (`useTexture ? texture : factor`; roughness remapped between a lower and upper bound) | `o3de/Gems/Atom/Feature/Common/Assets/Shaders/Materials/MaterialInputs/{BaseColor,Metallic,Roughness}Input.azsli` |
| Unreal (Interchange glTF) | the importer fills a master material's BaseColorFactor, MetallicFactor, RoughnessFactor, EmissiveFactor (defaults 1, 1, 1, 0); the multiply is in the master material asset | `UnrealEngine/Engine/Plugins/Interchange/Runtime/Source/Import/Private/Gltf/InterchangeGLTFMaterial.cpp:451-582` |

Majority (glTF, Filament, Bevy, Godot's separate maps, Unreal's glTF): **factor × texture, a missing texture is white**. The exceptions:
O3DE replaces (texture or factor), and Godot's packed ORM mode has no factors at all.

## Proposal

The default PBR material becomes a graph on `Output: PBR` with exposed factors (instance parameters):

```
Base Color = BaseColorFactor (COLOR, 1)      * Albedo.rgb
Roughness  = RoughnessFactor (FLOAT, 1)      * ORME.g
Metallic   = MetallicFactor  (FLOAT, 1)      * ORME.b
AO         = 1 + AOStrength  (FLOAT, 1)      * (ORME.r - 1)        (Filament's form)
Emissive   = EmissiveColor   (COLOR, 1)      * ORME.a * Albedo.rgb (today's mb_material_pbr.glsl: ORME.a * Albedo)
Normal     = Normal Map step, Strength (FLOAT, 1)
```

- **No ORME map:** the instance points at one shared white ORME texture (not one per material); the importer writes the model's values
  (roughness from shininess, metallic, emissive colour) into the factors.
- **An ORME map:** the factors stay 1, the map shows as authored (with factors, unlike Godot's ORM mode, so an instance can still tweak it).
- **No albedo map:** a shared white albedo, the diffuse colour into BaseColorFactor (the magenta placeholder for a missing file stays).
- The alpha-test variant gets the same factors plus Alpha Cutoff (part one, Masked).

Decided (2026-10-22): emission keeps today's rule, EmissiveColor × ORME.a × Albedo, never a separate emissive map: fewer textures is the
goal, and the ORME layout already carries it.

# Part three: the material instance (built 2026-10-23)

Agreed with the owner 2026-10-22.

- **One section.** The instance's inspector is its descriptor only. The material's parameters are its properties (`Parameters`:
  Textures, Colors, Values, Vectors2/3/4, by name): the value this instance sets is marked and `>` goes back to inheriting it; a
  parent's value or the material's default shows unmarked. The view is filled from the resolved instance, never saved; an edit is a
  `SetOverride`, a revert a `ClearOverride`. The hand-made Parameters panel is gone. xproperty grew `vector4_group` and `color_group`
  (four cells on one row; a colour also shows its swatch) for these rows.
- **Base.** One field takes a Material or a Material Instance (a variant of it): picking runs `SetMaterial` or `SetParent`. With an
  instance as the base, MaterialRef shows read only (it is the base's); ParentRef is not shown. The chain is resolved by the compiler:
  the compiled instance has one block and one texture list, no chain at runtime.
- **Render Settings section:** an instance shows the material's system parameters (not the graph's) under Base, in a section of their own:
  Double Sided, and Alpha Cutoff for a Masked material. Both are overrides like the parameters (marked when the instance sets them, `>`
  reverts). The material compiler declares them: Alpha Cutoff with storage (the shader reads it), Double Sided STATIC (no storage, the
  layout hash is unchanged), default the material's setting (`xmaterial::double_sided_param_id_v`). The instance compiler resolves it
  through the chain into the instance data file (v3 `m_DoubleSided`: -1 the material's, else the value), mapped by
  `xmaterial::RenderState(Settings, DoubleSided)`. The back-face normal flip is always compiled in (`gl_FrontFacing ? N : -N`, never taken
  while back faces are culled), so double sided is pipeline state only: no second shader. Back Face Normal "None" stays a material setting
  (`XMAT_BACK_FACE_AS_FRONT`). Each instance already has its own renderer material, so no pipeline cache was needed.
- **Order:** a material parameter has an `Order` (SetParamInfo -Order): an instance lists its parameters by it (ties as declared). Default
  PBR: each value under its texture.
- Alpha Cutoff was already an instance parameter (part one).

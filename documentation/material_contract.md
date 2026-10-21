# The material parameter contract

A material parameter has three separate things: a stable **ID** (who it is), its **value sources** (graph literal, instance, entity,
static), and its **GPU storage** (where the compiled shader reads it). IDs survive renames and repacking; offsets are compiled details
nobody stores. This doc is the contract between the graph, the compiler, material instances, the level and xLIONRender, and the plan
to build it.

`[UNVERIFIED]` marks an assumption the code did not confirm. `file:line` points at the code as it was when this was written.

```
source/xmaterial_params.h                                     (new) declarations, types, packer, manifest - standalone, no resources
plugins/xmaterial_instance.plugin/source/xmaterial_instance_resolve.h    (new) chain resolution - standalone (like xmaterial_instance_carry.h)
dependencies/xLIONRender/src/xlionrender_scene_components.h   the level side: Mesh (slot swaps, exists) + MaterialParams (new)
```

## 0. What the renderer does today (audit)

| | What | Where |
|---|---|---|
| Draw identity | One `draw` per Mesh entity; `m_Instance` low 32 bits = `uint32(Entity ^ Entity>>32) \| 1`. A hash: two entities can collide and share a slot (transform included) - pre-existing | `xlionrender_scene_renderer.h:174-175`, `scene/xlionrender_scene.h:56-67` |
| Slot swaps | **Already exist per entity**: `mesh::m_Materials` replaces the geometry's default instance per slot | `xlionrender_scene_components.h:43`, `xlionrender_scene_renderer.h:176-183` |
| Renderer material | One per material *instance* guid: HDR fragment shader + textures; its own pipelines per vertex path | `xlionrender_scene_renderer.h:113-131`, `xlionrender_instance.cpp:790-855` |
| Material values on GPU | None. Graph floats are baked as literals (`std::to_string`, 6 decimals) | `Compiler/xmaterial_compiler.cpp:169-189` |
| Per-draw data | Persistent instance SSBO (std430, 96 B: L2W rows + constant material), slots keyed by instance id, dirty-tracked, evicted LRU | `features/instances/xlionrender_instance_slots.h:38-58`, `plans/forward/xlionrender_plan_forward.h:32-45` |
| Upload | CPU ring (`ring_frames_v = 3`) -> `CopyBuffer` into device-local buffer, contiguous runs only | `xlionrender_plan_forward.cpp:60-113`, `xlionrender_instance.h:275-277` |
| Slot reaches shaders | Push constant `instanceIndex` (rigid 16 B, skin 12 B); every vertex path forwards it as `flat out uint OutInstance` at location 8 | `xlionrender_plan_forward.h:67-81`, `shaders/xlionrender_static_geom_body.glsl:29,85`, `xlionrender_skin_geom_vert.glsl:34,44`, `xlionrender_gi_capture_vert.glsl:23,46` |
| Binding model | Descriptor sets, no bindless: set 0 samplers (fragment only), set 1 static SSBOs, set 2 dynamic UBOs; max 16 per set. Static sets cached by buffer key; the span passed must equal the pipeline's static count | `xGPU/.../xgpu_vulkan_pipeline.h:35-37`, `xgpu_vulkan_pipeline.cpp:327-336,368-392`, `xgpu_vulkan_window.cpp:1412-1490` |
| Material layout | Samplers 0 shadow, 1-7 material textures, 8-10 env; SSBO 0 clusters, 1 instances (**vertex only**), 3-4 lights, 5-7 GI, (skin 2, 8) | `xlionrender_instance.cpp:204-225,822-842` |
| Instancing | None. One `Draw` per cluster per submesh, sorted by mesh then material; pipeline rebinds on material change | `xlionrender_plan_forward.cpp:1096-1190` |
| Constant material | Per-draw dynamic UBO entry rewritten every frame (`max_draws_per_frame_v = 16384`, flagged ponytail) | `xlionrender_instance.cpp:93,260`, `xlionrender_plan_forward.cpp:1129-1137` |
| Limits | Push constants <= 128 B (static_assert), both stages see them; UBO entries padded to `minUniformBufferOffsetAlignment` | `xgpu_cmd_buffer_inline.h:95-97`, `xgpu_vulkan_pipeline.cpp:451-465`, `xgpu_vulkan_buffer.cpp:38-45` |
| Invalidation | Renderer material re-made when any of instance/material/texture pointers change (recompile) | `xlionrender_scene_renderer.h:228-240` |
| Other consumers | Geom editor previews (`CreateMaterial` with HDR), material/instance editors' `xeditor::mesh_preview` (raw xGPU, **LDR** shader) | `xgeom_static_editor_preview.h:290`, `xgeom_skin_editor_preview.h:324`, `Editor/xmaterial_editor.h:149`, `source/Tools/Editor/xeditor_mesh_preview.h:121-136` |
| Level build | None found. Level save writes the ECS scene as is | `plugins/xlevel.plugin/source/Editor/xlevel_save.h:71-90` |

### Transport decision

**(a) Material constant block: one renderer-wide SSBO + a base index in the push constant.**
- `MaterialParams` (std430 `vec4 P[]`), device-local, set 1 binding 9, fragment-visible. Each renderer material gets a base (vec4 index)
  at `CreateMaterial`; its block is copied in once (staging + `CopyBuffer`), freed with the material.
- Base reaches the fragment as `layout(push_constant) uniform { layout(offset = 16) uint MaterialBase; }`. `push_static_geom` grows
  16 -> 20 B, `push_skin_geom` 12 -> 20 B (padded so the field sits at 16 in both: one fragment shader serves both paths).
- Cost: +4 B per cluster push (already pushed per cluster), +1 static descriptor (one buffer: the set cache key does not grow), 16 B per
  vec4 of parameters per material instance, zero per-frame CPU.
- Rejected: per-draw dynamic UBO (per-frame rewrite, what the constant material already regrets); a buffer per material in the static set
  (cache keys become mesh x material); values in push constants (128 B cap, rigid/skin layouts differ).

**(b) Entity block: a parallel SSBO indexed by the instance slot.**
- `EntityParams` (std430, 64 B stride), set 1 binding 10, fragment-visible, same index as `InstanceBuffer`; read with the existing
  `flat in uint InInstance` (location 8). Written by `instance_slots::Flush` through the same ring and `CopyBuffer` regions; the 64 B
  block joins `Acquire`'s change test.
- Cost: device 81920 slots x 64 B = 5 MiB; host ring 3 x 5 MiB = 15 MiB (sized like the instance ring; shrink to pending-only if it
  matters); upload only on change; entities without the component cost a zeroed slot.
- Rejected: growing `instance_data` 96 -> 160 B (every vertex/shadow read pays the stride, ring +23 MiB); push constants (per cluster,
  layouts differ); per-entity UBO.
- **Owner of entity data: the ECS entity's draw** (one instance slot). It applies to every submesh whose material opts into the field.
  Per-submesh targeting: deferred (header lane reserved). Instanced draws: none exist; when they come, the slot index comes from
  `gl_InstanceIndex + base` and nothing here changes.

**LDR variant** (editor previews only, `data_file.h` "drop the LDR one after"): compiled with `MaterialBase = 0u` and no entity reads
(entity-overridable params read the material value). `mesh_preview` binds a one-material `MaterialParams` at binding 9.

## 1. Data structures

Sketches; xproperty-friendly (vectors of structs, explicit IDs as `u64` fields, no maps - see §8).

```cpp
namespace xmaterial
{
    using param_id = std::uint64_t;                     // guid_generator::Instance64() at promotion; 0 = none. Never reused.

    enum class param_type : std::uint8_t { FLOAT, FLOAT2, FLOAT3, FLOAT4, COLOR, TEXTURE };   // v1. INT/BOOL: §7. Matrices, arrays: deferred
    enum class param_kind : std::uint8_t { DYNAMIC, STATIC /*reserved: switches, §7*/ };
    enum class param_stage: std::uint8_t { FRAGMENT = 1, VERTEX = 2 /*reserved; xGPU samplers are fragment-only today*/ };
    enum class entity_field: std::uint8_t { NONE, TINT, PARAM0, PARAM1 };                    // the v1 shared schema, §4

    struct float4v { float X = 0, Y = 0, Z = 0, W = 0; };   // xproperty has no fvec4 (§8): four named floats

    struct param_decl                                   // lives in the graph file, one per parameter
    {
        param_id        m_ID;
        std::string     m_Name;                         // display + carry-over fallback; renaming never touches instances
        param_type      m_Type;
        param_kind      m_Kind          = param_kind::DYNAMIC;
        std::uint8_t    m_Stages        = std::uint8_t(param_stage::FRAGMENT);
        float4v         m_Default;                      // linear (§3). TEXTURE: unused
        xresource::full_guid m_DefaultTexture;          // TEXTURE only
        float4v         m_ValidMin, m_ValidMax;         // clamped at resolve, diagnostic when clamped. Unbounded: +-FLT_MAX
        float4v         m_UiMin,    m_UiMax;            // slider range only; values outside it are legal
        bool            m_bHDR          = false;        // COLOR: rgb may exceed 1
        bool            m_bAlpha        = true;         // COLOR: w is meaningful
        entity_field    m_EntityField   = entity_field::NONE;   // "allow entity override": which schema field may replace it
        std::string     m_Group;                        // inspector grouping
    };

    // A node input that reads a parameter instead of its literal (several inputs may share one m_ParamID)
    // Stored on node_param (Graph/xmaterial_graph_node.h:46-50): add  param_id m_ParamID = 0;  m_bExpose/m_ExposeName become derived.
}

namespace xmaterial_instance
{
    struct override_entry                               // sparse: present = set (even if equal to the default); absent = inherit
    {
        xmaterial::param_id     m_ID;
        xmaterial::param_type   m_Type;                 // what it was set as: a retyped declaration leaves it ORPHAN, value kept
        xmaterial::float4v      m_Value;
        xrsc::texture_ref       m_Texture;              // TEXTURE only
        std::string             m_NameHint;             // the name when set: carry-over fallback + readable diffs. Not a key
    };

    struct descriptor                                   // replaces Textures/TextureDefaults/FinalTextures (§6 reads old files)
    {
        int                             m_Version   = 2;
        xrsc::material_ref              m_MaterialRef;  // the root material (derived from the parent when m_ParentRef is set)
        xrsc::material_instance_ref     m_ParentRef;    // optional: inherit from another instance
        std::vector<override_entry>     m_Overrides;    // sorted by m_ID on save (stable diffs)
    };

    struct data_file /* v2 */                           // compiled: the instance is fully resolved, no chain at runtime
    {
        xrsc::material_ref  m_MaterialRef;
        texture*            m_pTextureList;  std::uint8_t m_nTexturesList;     // as today: final texture slots in sampler order
        xmath::fvec4*       m_pParams;       std::uint16_t m_nParamVec4;       // the packed block (manifest order)
        std::uint64_t       m_LayoutHash;                                       // must equal the material's
    };
}

namespace xlionrender
{
    struct entity_params_v1                             // EntityParams entry, std430, exactly 64 B
    {
        std::uint32_t   m_Mask;                         // bit (entity_field - 1): the field is set
        std::uint32_t   m_SlotMask  = 0;                // reserved (per-slot targeting), must be 0 in v1
        std::uint32_t   m_Pad[2]    = {};
        xmath::fvec4    m_Tint;                         // linear rgba
        xmath::fvec4    m_Param0, m_Param1;
    };
    static_assert(sizeof(entity_params_v1) == 64);

    struct material_params                              // the level component ("MaterialParams"), next to Mesh
    {
        std::uint32_t   m_Mask = 0;                     // which fields are set (inspector checkboxes)
        xmath::fvec3    m_TintRGB = {1,1,1}; float m_TintA = 1;       // fvec3 + float: xproperty has fvec3, not fvec4
        float4v         m_Param0, m_Param1;
        // Slot swaps stay on Mesh::m_Materials (exists). Baked edits: deferred (§7, no level build step exists).
    };
}
```

## 2. The layout manifest

The material compiler emits it once; the editor, the instance compiler and the runtime read it. Nothing else computes offsets.

```cpp
struct manifest_entry { param_id m_ID; param_type m_Type; std::uint16_t m_ByteOffset;   // into the block; TEXTURE: sampler slot instead
                        std::uint8_t m_Sampler; entity_field m_EntityField; std::uint8_t m_Stages; };
struct manifest       { int m_Version = 1; std::uint64_t m_LayoutHash; std::uint16_t m_BlockBytes;
                        std::vector<manifest_entry> m_Entries; std::vector<param_decl> m_Decls; /* defaults, ranges */ };
```

- Written as `MaterialLayout.txt` beside today's template (`<ResourcesLogs>/<material>.log/`, `Compiler/xmaterial_compiler.cpp:537`);
  the instance compiler already takes that folder's template as an asset dependency (`xmaterial_instance_compiler.cpp:87-100`).
  `MaterialInstance.txt` (the template) is retired once §6 lands.
- `m_LayoutHash` (and block bytes) also go into the material `data_file` (v3) for the runtime check.

**Packing (std430 == std140 here, the block is `vec4 P[]`):**
1. Only DYNAMIC, non-TEXTURE params that some connected input reads. Declared-but-unused params get no storage (diagnostic `W_UNUSED`).
2. Order: by type size descending (FLOAT4/COLOR, FLOAT3, FLOAT2, FLOAT), then by ID. Never by name (renames must not repack).
3. FLOAT3/FLOAT4/COLOR start on 16 B. FLOAT2 on 8 B, FLOAT on 4 B, never straddling a 16 B line. FLOAT may fill a FLOAT3's `.w`.
4. Block size rounded up to 16 B; an empty block is 0 bytes (no binding reads happen).
5. `m_LayoutHash` = FNV-1a 64 over `(ID, type, offset, entity field)` of every entry in manifest order, plus the block size
   `[UNVERIFIED: no shared 64-bit hash helper checked; any stable one will do, it is never persisted across hash changes]`.

**Shader emission** (at `Compiler/xmaterial_compiler.cpp:169-189`, "use param fallback"): an unconnected input whose `node_param` has
`m_ParamID` emits a read instead of a literal:
```glsl
// [TOP], once per material that has a block
layout(std430, set = 1, binding = 9) readonly buffer MaterialParamsBuf { vec4 P[]; } g_Mat;
#ifdef XLION_LDR_OUTPUT
    #define XMAT_BASE 0u
#else
    layout(push_constant) uniform XMatPush { layout(offset = 16) uint MaterialBase; } g_XMatPush;
    #define XMAT_BASE g_XMatPush.MaterialBase
    // + only when some param has an entity field:
    layout(std430, set = 1, binding = 10) readonly buffer EntityParamsBuf { uvec4 H; vec4 Tint; vec4 P0; vec4 P1; } g_Ent[];   // [UNVERIFIED: struct array form]
    layout(location = 8) flat in uint InInstance;
#endif
// a FLOAT at byte 20:            g_Mat.P[XMAT_BASE + 1u].y
// a COLOR with entity field TINT: ((g_Ent[InInstance].H.x & 1u) != 0u ? g_Ent[InInstance].Tint : g_Mat.P[XMAT_BASE + 0u])
```
Literals that stay literals are written with `std::format("{}")` (round-trip), not `std::to_string`.

**Bindings fixed by this contract:** set 1 binding 9 `MaterialParams`, binding 10 `EntityParams`, push offset 16 `MaterialBase`, samplers
as today (`$tex[n]` -> sampler 1..7). Every material pipeline (rigid, skin, GI capture) declares 9 and 10 as fragment SSBOs whether or
not the shader reads them, so one layout serves all materials.

## 3. Values, color space, defaults

- Every value is **linear** everywhere: graph file, instance file, manifest, GPU. The inspector's color picker shows sRGB and converts
  on edit; typed numbers are linear. No shader-side conversion for parameters.
- Textures: sRGB decode is the texture format's job at sampling `[UNVERIFIED: xtexture compiles albedo as an SRGB format]`.
- COLOR is a float4; `m_bAlpha = false` -> w forced to 1 at resolve; `m_bHDR = false` -> rgb clamped to [0,1] (part of validRange).
- `validRange` clamps at resolve (instance compile) and reports `W_CLAMPED`; `uiRange` only bounds sliders.
- Defaults live in the declaration. The compiled material block holds the defaults (the material's "own instance").

## 4. Entity parameters (v1 schema)

- One engine-wide schema: `entity_params_v1` (§1). A material opts in per parameter with `m_EntityField`.
- Compatibility (checked by the material compiler): TINT accepts COLOR/FLOAT4/FLOAT3; PARAM0/1 accept FLOAT..FLOAT4 (components beyond
  the param's are ignored). Anything else, or two params on one field, is `E_ENTITY_FIELD` and the field is dropped.
- Effective value = entity field if its mask bit is set, else the resolved instance value. Decided in the shader per pixel (one load,
  one select); no per-entity material copies.
- Component without a Mesh, or materials that do not opt in: the data uploads and nothing reads it (no error; inspector shows
  "no material on this entity uses Tint").
- Change detection: the 64 B block is compared in `instance_slots::Acquire` with L2W and the constant material.
- GI: the capture draws through the same material with the draw's slot (`xlionrender_instance.h:244`, `m_Slot`), so the first capture
  sees the entity value. A later entity edit is not recaptured in v1 (capture jobs key on `m_Materials`) - known limit.

## 5. Resolution and invalidation

**Instance compile (asset build, never a shader compile):**
```
chain   = [this, parent, parent's parent, ...]           // stop at a material; depth > 8 -> E_CHAIN_DEPTH; repeat -> E_CHAIN_CYCLE
value   = manifest default
for I in reversed(chain): for e in I.Overrides with e.ID in manifest:
    if e.Type != decl.Type          -> W_ORPHAN_TYPE (kept, not applied)
    else value[e.ID] = e.Value
for e not in manifest               -> W_ORPHAN_ID   (kept, not applied, shown greyed in the inspector)
clamp to validRange                 -> W_CLAMPED
pack by manifest offsets -> data_file.m_pParams; textures -> m_pTextureList (sampler order); m_LayoutHash = manifest hash
```
- Every ancestor descriptor and the material's `MaterialLayout.txt` are dependencies, so a parent or material edit rebuilds descendants
  `[UNVERIFIED: xresource_pipeline recompiles dependents when an m_Dependencies.m_Assets file changes - the template mechanism relies
  on it today]`.
- Parent default changes are inherited by children that did not set the value. Only a migration with `PreserveLook` writes overrides (§6).
- Unset is distinct from set-to-default: `ClearOverride` removes the entry; `SetOverride` with the default value keeps it (pinned).

**Runtime:**
- `MaterialOf` (`xlionrender_scene_renderer.h:113`) passes the instance block to `CreateMaterial(..., std::span<const fvec4> Params)`.
- Instance `m_LayoutHash != material m_LayoutHash` (a stale instance against a recompiled material) -> `E_LAYOUT_STALE` once per
  instance, the slot draws with the constant material. Never read a block with the wrong layout.
- Recompile of either side already re-makes the renderer material (pointer change, `xlionrender_scene_renderer.h:228-240`); its
  `MaterialParams` range is freed and re-allocated.

## 6. Versioning and migration ops

Graph-side commands (undoable, `<resource>\<Command>` as in `editor.md`):

| Command | |
|---|---|
| `ExposeParam -Node -Param [-Id hex] [-Name]` | an unconnected input becomes a parameter (new ID, default = its literal); `-Id` attaches it to an existing one (several inputs, one param; types must match) |
| `UnexposeParam -Node -Param` | the input is a literal again (value = the param's default). The declaration stays (`W_UNUSED`) |
| `SetParamInfo -Id [-Name -Group -UiMin -UiMax -ValidMin -ValidMax -HDR -Alpha -EntityField]` | metadata; `-Name` is a rename: instances untouched |
| `SetParamDefault -Id -Value -Policy Inherit\|PreserveLook` | Inherit: children follow. PreserveLook: every instance whose parent is the material and has no override gets the *old* default written (deeper instances inherit it through them) |
| `RetypeParam -Id -Type [-Convert Splat\|Truncate]` | ID kept; overrides of the old type are converted when `-Convert` is given and the instance files are rewritten, else they become `W_ORPHAN_TYPE` |
| `PurgeUnusedParams` | deletes declarations nothing reads (the only way they go) |
| `ListParams` | ID, name, type, readers, entity field, offset from the last manifest |

Instance-side (`Editor/xmaterial_instance_editor.h`): `SetOverride -Id -Value`, `ClearOverride -Id`, `SetParent -Instance hex`,
`ListOverrides` (with source: default / parent name / here / orphan).

**SetMaterial / SetParent carry-over:** by ID first; then by name for IDs the new root does not have (`MatchCarriedTextures` logic,
`xmaterial_instance_carry.h`, generalised to all types); the rest are kept as orphans, not dropped.

**Old files** (`m_Version` absent = 1): `Textures[i]` that differ from the template's default become TEXTURE overrides keyed by the
param ID the compiler gives the texture's existing pin (`TextureDefaults[].GUID` = the input pin guid, `xmaterial_compiler.cpp:517-523`);
v1 materials get param IDs for their exposed textures = that pin guid, so old instances map without a lookup table. Written back as v2
on save only.

## 7. Deferred (with the hook left in v1)

| | Hook |
|---|---|
| Static switches | `param_kind::STATIC`; a bounded set per material, permutations compiled only for combinations instances request |
| INT / BOOL | need a representation decision (int bits in a vec4 lane vs float); STATIC bools come with switches |
| Matrices, resource arrays | none |
| Vertex stage | `m_Stages`; needs the material SSBO visible to the vertex stage and vertex-stage samplers (xGPU sets samplers fragment-only, `xgpu_vulkan_pipeline.cpp:336`) |
| Baked per-entity edits (hidden instances) | needs a level build step (none exists). Dedup key when it comes: hash of resolved block + texture list + material guid |
| Per-slot entity targeting | `entity_params_v1::m_SlotMask` (needs the slot index in the fragment: push it next to `MaterialBase`) |
| GI recapture on entity edit | key capture jobs on the entity block too |

## 8. xtextfile / xproperty limits found

1. Unknown paths are dropped silently on read: `setProperty`'s error string is ignored and the check tests the outer `Err`
   (`xproperty/source/sprop/property_sprop_xtextfile_serializer.h:207-215`). A renamed C++ member loses its data without a word.
   Hence overrides are generic rows (`ID, Type, Value`), never one C++ member per parameter.
2. No `fvec4` reflection (`xmath/source/bridge/xmath_to_xproperty.h:27-30`: fvec2, fvec3, fbbox, fquat). Use `float4v` or add
   `vec4_friend` (small task, optional).
3. `std::map<u64,T>` works but keys print decimal (`xcore/my_properties.h:591`, `%llu`) while `u64` fields print hex (`#6E`). Use a
   vector of entries with an `ID` field, sorted by ID on save.
4. No schema version in property streams: carry an explicit `Version` member. Custom-record files (the graph) can append optional
   records: `ReadRecord`/`getRecordName`/`isEOF` exist (`xtextfile/source/xtextfile.h:307-325`) `[UNVERIFIED: how an old reader reacts to
   an unknown trailing record]`.
5. Nested lists are fine (paths like `A[G:0]/B[G:1]/X`); each list costs a `[]` count row.

## 9. File examples

Graph file: a `Parameter` record per declaration after the connections (`Graph/xmaterial_graph.cpp:225-330` writes Info/Node/Connection
the same way), then its property rows. The node param row gains `ParamID`.
```
[ Info ]
{ nNodes:d  nConnections:d  nParameters:d }
  6          5               2
...
[ Parameter ]
{ Guid:G }
  #6A1F00C2D93B7701
[ xProperties : 12 ]
{ Name:s                               Value:?                     }
  "material_param/ID"                  ;u64    #6A1F00C2D93B7701
  "material_param/Name"                ;string "Tint"
  "material_param/Type"                ;enum   "COLOR"
  "material_param/Default/X"           ;f32    1.0
  "material_param/Default/Y"           ;f32    1.0
  "material_param/Default/Z"           ;f32    1.0
  "material_param/Default/W"           ;f32    1.0
  "material_param/HDR"                 ;bool   0
  "material_param/Alpha"               ;bool   0
  "material_param/EntityField"         ;enum   "TINT"
  "material_param/Group"               ;string "Surface"
  "material_param/Stages"              ;u8     #1
...
  "node/Params[G:0]/ParamID"           ;u64    #6A1F00C2D93B7701
```

Material instance with a parent and sparse overrides:
```
[ xProperties : 12 ]
{ Name:s                                                   Value:?                                        }
  "material_instance_descriptor/Version"                   ;s32       2
  "material_instance_descriptor/MaterialRef"               ;full_guid #A900F07070230003 #8B3C028882EA813D
  "material_instance_descriptor/ParentRef"                 ;full_guid #3B10F2D0A1C40001 #<MaterialInstance type>
  "material_instance_descriptor/Overrides[]"               ;s64       1
  "material_instance_descriptor/Overrides[G:0]/ID"         ;u64       #91C4D0E2B7A80003
  "material_instance_descriptor/Overrides[G:0]/Type"       ;enum      "FLOAT"
  "material_instance_descriptor/Overrides[G:0]/Value/X"    ;f32       0.9
  "material_instance_descriptor/Overrides[G:0]/Value/Y"    ;f32       0
  "material_instance_descriptor/Overrides[G:0]/Value/Z"    ;f32       0
  "material_instance_descriptor/Overrides[G:0]/Value/W"    ;f32       0
  "material_instance_descriptor/Overrides[G:0]/NameHint"   ;string    "Roughness"
```

Level entity (scene file rows `[UNVERIFIED: exact scene row prefix]`):
```
  "Mesh/Geom"                     ;full_guid #...
  "Mesh/Materials[]"              ;s64   1
  "Mesh/Materials[G:0]"           ;full_guid #<MI_Red>
  "MaterialParams/Mask"           ;u32   #1
  "MaterialParams/TintRGB/X"      ;f32   0.0
  "MaterialParams/TintRGB/Y"      ;f32   1.0
  "MaterialParams/TintRGB/Z"      ;f32   0.0
  "MaterialParams/TintA"          ;f32   1.0
```

## 10. Diagnostics

Reported by the compilers (`LogMessage`), the editors (`CompileStatus`, inspector badges) and the renderer (once per resource).

| Code | Where | Meaning / effect |
|---|---|---|
| `W_UNUSED` | material compile | declaration nothing reads; kept, no storage |
| `E_PARAM_TYPE` | material compile | inputs sharing an ID disagree on type; compile fails |
| `E_ENTITY_FIELD` | material compile | field incompatible or taken twice; entity override dropped |
| `E_TOO_MANY_TEXTURES` | material compile | > 7 material textures (samplers 1-7, `xlionrender_instance.cpp:820`) |
| `W_ORPHAN_ID` / `W_ORPHAN_TYPE` | instance compile | override kept, not applied |
| `W_CLAMPED` | instance compile | value outside validRange |
| `E_CHAIN_CYCLE` / `E_CHAIN_DEPTH` | instance compile | compile fails |
| `E_PARENT_ROOT` | instance compile | parent's root material differs from `MaterialRef`; compile fails |
| `E_LAYOUT_STALE` | renderer | instance and material hashes differ; slot draws the constant material |
| `W_ENTITY_UNUSED` | level inspector | a set entity field no material on the entity reads |

## 11. Vertical slice

Material `SliceMat`: sampler `Albedo` (exposed TEXTURE), `Tint` (COLOR, no alpha, entity field TINT, default 1,1,1), `Roughness`
(FLOAT, valid [0,1], default 0.5). Output base color = `Albedo.rgb * Tint.rgb`, roughness = `Roughness`.
Instances: `MI_Red` (parent SliceMat, Tint = 1,0,0), `MI_RedRough` (parent MI_Red, Roughness = 0.9). Level: cube A with `MI_Red`, cube B
with `MI_Red` + `MaterialParams` Tint = 0,1,0, cube C with `MI_RedRough`. White albedo texture.

Acceptance tests:
1. Packer self-test (`xmaterial_params.h` `demo()` asserts): COLOR@0, FLOAT@12 (fills .w), hash stable across a rename, changes on retype.
2. Manifest of SliceMat lists 3 entries, block 16 B, Tint entity field TINT; `SetParamInfo -Name` leaves the hash unchanged.
3. Resolver self-test: MI_RedRough resolves to Tint 1,0,0 / Roughness 0.9; ClearOverride on MI_Red's Tint -> MI_RedRough Tint 1,1,1;
   a cycle MI_A -> MI_B -> MI_A is `E_CHAIN_CYCLE`; an override of an unknown ID survives a save/load round trip.
4. `SetParamDefault Roughness 0.2 -Policy PreserveLook`: MI_Red gets Roughness 0.5 written, MI_RedRough unchanged; A and C render as before.
5. Render (extend `tests/xlionrender_test_forward.cpp`): A center pixel red-dominant, B green-dominant, C red with a broader highlight
   than A; the same frame with B's mask cleared makes B match A within tolerance.
6. Editing B's tint in the level re-uploads one 64 B slot (stats: pending count 1), no material re-made (`m_Materials` count unchanged).
7. Recompile SliceMat with a new param: stale MI files report `E_LAYOUT_STALE` until rebuilt, nothing reads a wrong block; after rebuild
   all three render as before.
8. Old v1 instance file with a changed texture loads as one TEXTURE override with the right ID.

## 12. Implementation tasks (in order)

Each task leaves the build green and the existing materials rendering unchanged unless it says otherwise.

1. **Param types + packer + manifest (standalone).** `source/xmaterial_params.h` (new). `param_decl`, `manifest`, `Pack()`, `Hash()`,
   text I/O via xproperty. Done: `demo()` asserts of test 1 pass; no other file touched.
2. **Graph declarations.** `Graph/xmaterial_graph.h/.cpp` (Parameter records, `nParameters`), `Graph/xmaterial_graph_node.h`
   (`m_ParamID`). Old graphs load (no records = none); save/load round-trips declarations. Done: an existing material file loads and
   saves byte-identical apart from the new count.
3. **Graph commands.** `Editor/xmaterial_graph_editing.h`, `Editor/xmaterial_editor.h`: Expose/Unexpose/SetParamInfo/ListParams (undoable),
   Node Properties shows the param link. Done: commands work from the console; undo restores the literal.
4. **Compiler emission.** `Compiler/xmaterial_compiler.cpp`: param reads at the fallback site, `[TOP]` block (§2), `MaterialLayout.txt`,
   entity field checks, diagnostics, round-trip float literals. `xmaterial_data_file.h` v3 (+hash, block bytes, default block).
   Done: test 2; a material with no params compiles to the same shader as before.
5. **Renderer: MaterialParams.** `xlionrender_instance.h/.cpp` (buffer, range allocator, bindings 9/10 in all material pipelines,
   `CreateMaterial` span arg), `plans/forward/xlionrender_plan_forward.h/.cpp` (push structs to 20 B, `MaterialBase`, static spans 7->9 /
   9->11, GI capture spans), `xlionrender_scene_renderer.h`, both geom editor previews. Binding 10 bound to a 1-entry dummy until task 9.
   Done: existing scenes render identically; SliceMat with defaults renders white x albedo.
6. **LDR preview.** `source/Tools/Editor/xeditor_mesh_preview.h`, `Editor/xmaterial_editor.h`: binding 9 with the material's default block.
   Done: material editor preview shows the default Tint.
7. **Resolver (standalone).** `xmaterial_instance_resolve.h` (new): chain walk, orphan/clamp rules, PreserveLook helper. Done: test 3 asserts.
8. **Instance descriptor + compiler.** `xmaterial_intance_descriptor.h` (v2, old-file read), `Compiler/xmaterial_instance_compiler.cpp`
   (resolve, dependencies on ancestors + manifest), `xmaterial_instance_data_file.h` v2, renderer hash check. Done: tests 7, 8.
9. **Instance editor.** `Editor/xmaterial_instance_editor.h`: override inspector (inherit/here/orphan), Set/Clear/SetParent/List commands,
   carry-over by ID then name (`xmaterial_instance_carry.h`). Done: MI_Red/MI_RedRough authored from the console.
10. **Entity params.** `xlionrender_scene_components.h` (`material_params` + registration in `xlionrender_plugin_entry.cpp`),
    `xlionrender_scene_renderer.h` (fill the block per draw), `scene/xlionrender_scene.h` (`draw::m_EntityParams`),
    `features/instances/xlionrender_instance_slots.h` (block in slot + change test), `xlionrender_plan_forward.cpp` (EntityParams ring +
    copy), `xlionrender_instance.cpp` (buffers). Done: test 6; entities without the component unchanged.
11. **Vertical slice + render test.** Assets in `example.lionprj`, `tests/xlionrender_test_forward.cpp` probes. Done: test 5.
12. **Migration ops.** `SetParamDefault -Policy`, `RetypeParam`, `PurgeUnusedParams` (graph editing; rewrites instance files through the
    descriptor API). Done: test 4.

## Open questions for the owner

- One level component was asked for; slot swaps already ship on `Mesh::m_Materials`. v1 here keeps them there and adds `MaterialParams`
  for entity values. Move slot swaps into it (scene migration) or keep?
- "Baked at level build" needs a level build step that does not exist. Build one, or bake at level save?
- Entity data is per entity (all submeshes). Is per-slot targeting needed in v1?
- Files store linear colors (§3). Prefer authored sRGB in files instead?
- The 32-bit instance-id hash (`xlionrender_scene_renderer.h:175`) can make two entities share a slot - now also their entity
  parameters. Fix before or with task 10?

## Owner decisions (2026-10-21)

- Slot swaps may move: v1 consolidates them into one material component with the entity parameters (task 10 migrates
  `Mesh::m_Materials`), for clarity rather than keeping two places.
- Baking per-entity edits stays deferred until a level build step exists.
- Colors are linear everywhere (files, GPU, HDR).
- Fix the 32-bit instance-id hash collision before task 10.
- 64-bit keys are `std::uint64_t` (they print as hex); fvec4 values may be written as plain user fields instead of through xproperty.
- Fields can be reordered freely in xtextfile; a new version simply stops reading old fields.

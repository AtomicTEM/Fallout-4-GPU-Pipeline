# Architecture

This document describes how the plugin removes per-object draw work from
Fallout 4's CPU render path, which engine behaviour it relies on, and how it
verifies that behaviour before changing a single frame.

## 1. The renderer as the plugin sees it

Fallout 4 renders each *view* (the main camera, every shadow cascade or
shadow-casting light, reflections, the local map) with a `BSShaderAccumulator`:

```text
accumulator->StartAccumulating(camera)          // vtable 0x28
scene graph cull (frustum, PreVis, occlusion planes, rooms/portals)
   for each visible BSGeometry:
       property->GetRenderPasses(geom, mode, accumulator)              // 0x2B
       property->GetRenderPasses_ShadowMapOrMask(geom, mode, acc)      // 0x2C
       property->GetRenderDepthPass(geom)                              // 0x30
accumulator->FinishAccumulating[PreResolveDepth|PostResolveDepth]()   // 0x29/0x2E/0x2F
   for each registered BSRenderPass:
       shader->SetupGeometry(pass)      // BSLightingShader / BSUtilityShader, slot 7
       context->DrawIndexed(...)        // ID3D11DeviceContext slot 12
       shader->RestoreGeometry(pass)    // slot 8
```

Every visible object costs a pass registration, the engine's state setup and
a D3D11 draw, all on the CPU. The plugin keeps the first half (the engine still
decides which objects exist and are visible) and replaces the second half for
batched objects.

All hooks are **vtable patches**:

- The engine vtables come from CommonLibF4RD's runtime database
  (`RE::VTABLE::*`). The primary vtable is picked by its RTTI Complete Object
  Locator.
- The D3D11/DXGI COM vtables come from the device objects themselves.
- One import-table patch on `Fallout4.exe` (`D3D11CreateDevice*`) sees the
  device before the renderer creates its input layouts.

No executable code is patched and no raw addresses are used.

## 2. Components

| Area | Files | Role |
| --- | --- | --- |
| Entry | `src/main.cpp`, `src/Plugin.*`, `src/Settings.*` | F4SE metadata, logging, INI |
| Engine hooks | `src/Engine/EngineHooks.*`, `src/Engine/Layouts.h` | Vtable hooks; field offsets the plugin reads |
| D3D hooks | `src/Render/D3DHooks.*`, `src/Render/InputLayouts.*` | Device creation, input layout capture, draw interception, Present/Present1 |
| Orchestration | `src/Core/Pipeline.*` | Receives every hook and owns all subsystems |
| Verification | `src/Core/Calibration.*` | Runtime checks that gate batching |
| Scene | `src/Scene/ObjectRegistry.*` | Per-geometry state machine |
| | `src/Scene/ViewRegistry.*` | Per-accumulator epochs and visibility lists |
| | `src/Scene/BucketManager.*` | Grouping, GPU merging and batch lifetime |
| | `src/Scene/VertexFormat.*`, `src/Scene/MergeMath.*` | Vertex format plans and member→anchor transforms |
| GPU | `src/Render/BatchRenderer.*` | Per-view culling dispatch and replacement draws |
| | `src/Render/HiZ.*` | Depth pyramid |
| | `src/Render/GpuBuffers.*` | Arenas and upload buffers |
| | `src/Render/NvApi.*` | NVAPI multi-draw |
| | `src/Render/ShaderLibrary.*` | Compute shaders: bytecode precompiled by `fxc`, or runtime HLSL compilation for overrides |
| Shaders | `shaders/MergeVertices.hlsl`, `MergeIndices.hlsl`, `Cull.hlsl`, `HiZ.hlsl` | `cs_5_0` compute |

## 3. Object lifecycle

Each eligible geometry gets an `ObjectRecord` (`src/Scene/ObjectRegistry.h`):

```text
Tracking ──(transform unchanged for iStableFrames)──► Candidate
Candidate ──(drawn by BSLightingShader; buffers captured)──► Captured
Captured ──(group settles, GPU merge done)──► Member
Member ──(moved / mesh or material changed)──► Evicted ──► record replaced
Captured/Candidate ──(unsupported format, bad capture)──► Rejected (retried later)
```

*Eligible* means:

- the vtable is exactly `BSTriShape`
- there is no skin instance
- the vertex format has no skinning, landscape or eye data
- there is no alpha blending (alpha testing is fine)

**Capture** happens inside the engine's own draw of the object. The plugin
records:

- the bound vertex buffer, stride and offset
- the index buffer, format and offset
- the draw arguments
- the input layout

No engine buffer structure is parsed. The vertex descriptor
(`BSGeometry+0x150`) and the input layout are cross-checked to build a
`VertexPlan`, and anything inconsistent is rejected.

## 4. Batches ("buckets")

Captured records are grouped at capture time by:

- material pointer, shader flags and alpha state
- vertex descriptor and stride
- shadow-caster flag
- grid cell (`fGridSize`, 4096 units by default)

The engine's own `BSShaderProperty::CanMerge`, the test the Creation Kit uses
for precombines, must also accept the pair. Groups that stop growing for
`iSettleFrames` are split by `iMaxMembersPerBucket`/`iMaxVerticesPerBucket`
and merged on the GPU:

1. The source vertex and index ranges are copied (`CopySubresourceRegion`)
   into a raw staging buffer, within a per-frame vertex budget.
2. `MergeVertices.hlsl` rewrites each vertex into the **anchor's local space**:
   `p' = inverse(anchorWorld) * memberWorld * p`.
   - Positions are widened to `float4`; half precision would crack seams
     thousands of units from the anchor.
   - Normals and tangents are rotated. So is the bitangent that the Creation
     Engine stores across the `.w` components of position, normal and tangent.
   - Members oriented like the anchor are copied bit-exactly.
3. `MergeIndices.hlsl` writes 32-bit, batch-local indices and flips the winding
   for mirrored transforms. It counts out-of-range indices, which are read back
   asynchronously; a non-zero count retires the batch.

The merged data lives in growable arenas: vertices (VB + raw UAV), indices
(IB + raw SRV/UAV), and a structured member table holding the bounding sphere
and index range of every member.

The anchor is the first member. The plugin holds a reference on it. The batch
is retired when any of these happen:

- the anchor detaches from the scene graph
- the anchor moves or changes material
- a quarter of the members are evicted
- draws become inconsistent
- calibration revokes batching

Storage is freed eight frames after retirement, so in-flight work never sees
freed memory.

## 5. One frame

```text
StartAccumulating(A)        new epoch E for view A; clear A's list
GetRenderPasses(member m)   append m to list(A, E)
  first member of batch B   gate[B][A] := E  → return the anchor's passes ("carrier")
  any other member of B     return an empty pass list ("suppressed")
GetRenderDepthPass(m)       paired with the decision above: anchor's depth pass / none
FinishAccumulating(A)       thread-local view stack = (A, E)
  SetupGeometry(anchor pass)      engine binds its shaders, material, CBs, layout
  DrawIndexed(anchor)             ← intercepted
     first batch draw in (A, E):  upload list(A, E); one Cull.hlsl dispatch for the view
     bind widened input layout + vertex arena (offset = batch) + compaction ring
     DrawIndexedInstancedIndirect(args[B])     ← replaces the engine's draw
  ...
FinishAccumulatingPreResolveDepth(main view) exit:
     copy depth → build Hi-Z (used by the next frame's main view)
Present:   calibration, batch maintenance (merge, retire, free), statistics
```

These properties make the substitution safe:

- **The engine still decides visibility.** Only members the engine registered
  for the view are drawn, so disabled, unloaded, PreVis-culled or
  frustum-culled objects are never drawn by the batch.
- **One carrier per (batch, view, epoch).** The gate is an atomic
  compare-exchange on the epoch, so an anchor's `BSRenderPass` objects are
  never registered twice in one accumulation. The anchor itself goes through
  the same gate.
- **Engine state is untouched.** The replacement draw runs with the engine's
  shaders, material, constant buffers and render states. Only the input layout,
  vertex/index buffers and the draw call change. Compute and IA bindings are
  saved and restored around every insertion (`StateGuards.h`), because the
  engine caches its D3D11 state.
- **Matching transforms.** The engine sets the anchor's world (and previous
  world) matrix. Merged vertices are in anchor space, so every member lands
  where it would have been drawn, and motion vectors for TAA remain correct for
  static geometry.

## 6. GPU culling (`shaders/Cull.hlsl`)

Each work item is a member the engine registered for the view. For the main
view (when `bOcclusionCulling=1`), the member's bounding sphere is projected
with the **previous frame's** camera matrix (`NiCamera::worldToCam`). It is
then tested against the Hi-Z pyramid built from that frame's depth:

- conservative max (or min for reversed-Z) reduction
- odd sizes fold into the last texel
- texel lookup by shifting mip-0 coordinates, which stays conservative for
  non-power-of-two sizes
- the 2×2 texel footprint at the smallest sufficient mip

Static geometry makes this temporal test exact apart from disocclusion. Camera
cuts (`fCameraCutDistance`) skip occlusion for a frame. This is Nvidium's
"last frame's visibility" idea expressed as Hi-Z instead of raster queries.

Two outputs are produced:

- **Compaction** (all GPUs): visible members' indices are copied into a ring
  buffer, contiguous per batch, and `IndexCountPerInstance` is accumulated with
  atomics. One `DrawIndexedInstancedIndirect` per batch draws it.
- **Per-member records** (NVIDIA, `iIndirectMode=2`/auto): one indirect record
  per member, with zero indices when culled. `NvAPI_D3D11_MultiDrawIndexedInstancedIndirect`
  draws the batch in one call.

Per view there is one upload and one dispatch, so UAV-to-IB hazards cost one
barrier per view, not one per batch.

## 7. Calibration (`src/Core/Calibration.cpp`)

Batching starts only after all of these hold over `iMinFrames`/`iMinSamples`:

| Check | Why |
| --- | --- |
| Learn where `BSRenderPass` keeps its geometry pointer: the only qword offset that equals the geometry in 100% of passes returned by `GetRenderPasses` | The layout is not in any public header; it is learned, then continuously re-verified |
| ≥99.9% of passes are rendered inside `FinishAccumulating*` | The view stack identifies which accumulation a draw belongs to |
| ≥99.9% of `SetupGeometry` calls are followed by a draw the D3D hook sees | Draws on unhooked (deferred) contexts would lose members |
| `worldBound == world * modelBound` confirms the `NiTransform` convention (or its transpose) | Merged vertices must match the engine's math |
| Depth-pass requests pair with the preceding registration | Otherwise only shadow views are batched, to avoid depth pre-pass mismatches |
| Render-mode histogram per hook; modes under 2% are never batched | Special views (local map, VATS) keep vanilla behaviour |
| Device hooks installed; input layouts known | Needed to bind the widened layout |

After enabling, these trigger revocation (all batches retired, vanilla
rendering for the session):

- contradictions in the learned offset
- batch anchors drawn outside an accumulator

The results are written to the log.

## 8. Threading

- Pass registration runs on the renderer's job threads. The object registry is
  a 64-way sharded map. View lists are lock-free appends. Gates are atomics.
  Hot counters are striped across cache lines.
- Draw interception, merging, Hi-Z and maintenance run on the render thread
  (Present).
- Records, batches and view storage are recycled several frames after they
  are retired, so a pointer taken inside a hook stays valid for that call.
- The anchor→batch map lets an anchor be recognised even if its registry
  record was replaced, so its passes are never registered twice.

## 9. Memory

Defaults (all in `[Memory]`):

- vertex arena: 512 MB maximum; grows from 32 MB as needed
- index arena: 192 MB
- member table: up to 1 M members
- index compaction ring: 64 MB
- indirect argument records: 1 M (20 MB)
- merge staging: 32 MB

When an arena is full, no new batches are created; affected objects render as
vanilla.

## 10. Known limitations and future work

- **Precombined meshes** (`BSMultiIndexTriShape`, `BSCombinedTriShape`) are
  left alone. The biggest win is in areas where precombines are disabled or
  broken, which is exactly where vanilla performance collapses.
- **Forward lights.** A carrier pass is built for the anchor, so per-object
  light lists of the anchor apply to the whole batch. Fallout 4 lights opaque
  geometry deferred, so this should not matter for the G-buffer, but it is the
  first thing to check if lighting differs. Disable the main view
  (`bBatchMainView=0`) to batch shadows only.
- **Fade.** Objects that are fading render individually. A batch whose anchor
  fades renders individually for those frames.
- **Mesh shaders.** Direct3D 11 has none. Moving the batch draw to Direct3D 12
  through D3D11On12 would allow a true Nvidium-style task/mesh shader path
  with per-meshlet culling. That is a possible next step once the D3D11 path is
  proven in game.
- **GPU occlusion in shadow views** is off; shadow cascades change every frame.
- **Draw-call statistics** are logged per frame. GPU-side cull counts would
  need an extra readback.

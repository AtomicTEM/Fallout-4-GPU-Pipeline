# Nvidium → Fallout 4: what carries over

[Nvidium](https://github.com/MCRcortex/nvidium) (and its AMD-capable
[`EXT_mesh_shader` fork](https://github.com/drouarb/nvidium/tree/EXT_mesh_shader))
replaces Sodium's chunk renderer in Minecraft with a GPU-driven one. This
plugin follows the same principles. The two engines differ a great deal,
though, so most mechanisms had to be translated rather than ported.

| Nvidium (OpenGL, Minecraft) | GPU World Pipeline (Direct3D 11, Fallout 4) |
| --- | --- |
| Hooks Sodium's chunk build output and uploads section geometry into one big GPU buffer (`SectionManager`, `BufferArena`) | Captures each static object's vertex/index buffers from the engine's own draw and merges them into GPU arenas (`BucketManager`, `ArenaBuffer`) |
| Terrain is defined by Nvidium's own compact vertex format and shaders | Materials, lighting and the G-buffer belong to the engine. Batches keep the engine's vertex format and are drawn **through the engine's shaders**, carried by one "anchor" object per batch |
| Regions/sections give spatial grouping | Grid cells (`fGridSize`) plus the engine's `CanMerge` material test give batches |
| Per-region CPU frustum test, then raster occlusion queries on section bounding boxes with `GL_NV_representative_fragment_test` and early fragment tests | The engine still does frustum, PreVis and occlusion-plane culling. A compute pass adds hierarchical-Z occlusion against the previous frame's depth |
| Temporal coherence: draw last frame's visible set first, test the rest, keep 8 frames of visibility history | Temporal Hi-Z: test static members against last frame's depth with last frame's camera. Camera cuts disable it for a frame |
| Task shaders read section visibility and emit mesh-shader work; mesh shaders emit quads (`glDrawMeshTasksIndirectNV`) | Direct3D 11 has no task/mesh shaders. A compute shader writes indirect draw arguments and either compacts index ranges (any GPU) or feeds `NvAPI_D3D11_MultiDrawIndexedInstancedIndirect` |
| Bindless pointers (`GL_NV_shader_buffer_load`, `NV_vertex_buffer_unified_memory`) | Raw/structured buffer views; one arena per data kind, so one SRV covers every batch |
| NVIDIA-only (Turing+); the fork extends this to AMD via `EXT_mesh_shader` | Index compaction runs on every Direct3D 11.0 GPU, while NVAPI multi-draw is an NVIDIA-only fast path. No vendor extension is required |
| Translucency sorted per region on the GPU | Alpha-blended objects are left to the engine |

## Why not mesh shaders

Fallout 4 renders with Direct3D 11, which has no mesh or task shaders and no
standard multi-draw-indirect. Matching Nvidium exactly would mean a
Direct3D 12 or Vulkan side renderer. That renderer would have to reproduce the
engine's lighting shaders and G-buffer encoding, or interoperate with them
through shared resources and D3D11On12 every frame. The current design instead
keeps every pixel produced by the engine's own shaders. Only *what gets drawn
and how it is submitted* changes. That is the part that costs CPU time.

A D3D11On12 mesh-shader backend is a possible later step (see
[ARCHITECTURE.md §10](ARCHITECTURE.md#10-known-limitations-and-future-work)):
per-meshlet culling would also remove GPU work that the per-object test keeps.

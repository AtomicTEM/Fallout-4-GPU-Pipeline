# Testing

## Automated (no game required)

| What | How | Covers |
| --- | --- | --- |
| Plugin build | CI `Plugin DLL (Windows, MSVC + vcpkg)` job, or `cmake --preset vs2022-windows-vcpkg && cmake --build --preset vs2022-release` | The plugin compiles with MSVC `/W4 /WX` against CommonLibF4RD |
| Package | CI `Verify package (Linux)` job, or `tools/check-package.sh <extracted package>` | `F4SE/Plugins/` layout with only the DLL and INI; a 64-bit DLL exporting `F4SEPlugin_Version`, `F4SEPlugin_Query` and `F4SEPlugin_Load`; no debug-CRT or third-party DLL imports |
| GPU tests on Windows | `-DBUILD_GPU_TESTS=ON`, then `build/vs2022/tests/Release/GpuTests.exe` | See below; uses WARP when no GPU is present |
| GPU tests on Linux | `tools/run-tests-wine.sh` | The same tests under Wine + Mesa llvmpipe, compiled by Microsoft's `d3dcompiler_47` |

`GpuTests` checks:

- **Range allocator:** alignment, padding reuse, growth and coalescing.
- **Merge transform:** member→anchor composition, direction matrices (exact
  identity for aligned members), mirrored-winding detection, degenerate
  rejection.
- **Vertex plans:** half- and full-precision Creation Engine formats,
  signed/unsigned normals, and rejection of skinned formats, stride mismatches
  and elements that split attributes.
- **`MergeVertices.hlsl`:** 97 random vertices with a half-precision position
  and a bitangent in the `.w` components. The anchor transform applied to the
  merged vertex must match the member transform applied to the source vertex
  (worst error ≈ 6e-5 units). Directions are rotated within byte precision and
  are bit-exact for aligned members.
- **`MergeIndices.hlsl`:** 16-bit indices at a 2-byte-aligned offset, 32-bit
  indices, winding flips, vertex base, out-of-range counting.
- **`Cull.hlsl` CSCompact:** per-batch contiguous compaction, argument
  accumulation, untouched neighbours.
- **`Cull.hlsl` CSMultiDraw + Hi-Z:** probes in front of, behind, straddling,
  outside and crossing the camera plane of an occluder.
- **`HiZ.hlsl`:** an odd-sized (37×23) gradient depth matches a CPU reference
  at every mip, and every mip-0 texel is conservatively covered.

The tests have been checked against deliberately broken shaders (a min instead
of max reduction, skipped rotation, a missing ring offset), and each mutation
is caught.

## In game

The renderer hooks can only be verified in Fallout 4. Recommended procedure:

1. **Observe first.** Set `[General] iMode=0`, start the game and load a
   save. In `GPUWorldPipeline.log` look for:
   - `hooks: ... slot ...` lines for all 11 engine hooks, and
     `d3d: device vtable hooked` (ideally before `pipeline: ready`)
   - `calibration: BSRenderPass geometry pointer at +0x..`
   - `calibration: batching on hold: ...`. In observe mode the plugin still
     reports what would block batching. Calibration completes in-world after a
     few seconds.
2. **Batch.** Set `iMode=1` and load the same save. After calibration you
   should see:
   - `calibration: all checks passed, batching allowed (main view: true|false, ...)` and a
     summary of render modes
   - every 10 s, `stats:` lines with active batches and members, plus
     `suppressed`, `carriers` and `replaced draws` per frame
3. **A/B.** Stand in a dense area (Diamond City market, Boston Common,
   Financial District, or any cell whose precombines are disabled by a mod)
   and press **F10** repeatedly. Compare:
   - frame rate and CPU frame time
   - the visual result: objects should not disappear, flicker, duplicate,
     shift, or show wrong normals or lighting
4. **Stress the lifecycle.**
   - Fast travel and cell transitions (batches retire when anchors detach)
   - Workshop mode: move, scrap and place objects (evictions)
   - Toggle the Pip-Boy map, VATS and photo mode (special render modes stay
     vanilla)
   - Quick camera turns (occlusion should not pop visibly)
   - Weather and time of day (shadows)

### What to report

Include the full log and the following:

| Symptom | Likely cause | Mitigation to try |
| --- | --- | --- |
| Batching never enables | The calibration line names the reason | Report the line |
| Objects missing in shadows only | Shadow-view carrier issue | `bBatchShadows=0` |
| Objects missing or flickering in the main view | Main-view routing or depth pre-pass pairing | `bBatchMainView=0` |
| Objects pop in during fast turns | Occlusion disocclusion | `bOcclusionCulling=0`, or raise `fOcclusionDepthBias` |
| Wrong lighting on batched objects | Anchor-specific per-object constants | `bBatchMainView=0`; report which objects |
| Distorted normals on rotated objects | Bitangent/normal encoding assumption | `bRotateBitangentW=0` |
| Stutter while new cells load | Merge budget | Lower `iIngestVertexBudget` |

`[General] bVerboseLogging=1` logs every rejected object and every new batch.
`[Culling] iIndirectMode=1` forces the vendor-neutral path on NVIDIA, to rule
out NVAPI.

# GPU World Pipeline for Fallout 4

An F4SE plugin that moves the draw calls for Fallout 4's static world geometry
from the CPU to GPU-driven batches, in the spirit of
[Nvidium](https://github.com/MCRcortex/nvidium) for Minecraft. It is built
on [CommonLibF4RD](https://github.com/Zzyxz/CommonLibF4RD), so one DLL loads
on the OG (1.10.163), NG (1.10.984) and AE (1.11.x) runtimes.

> **Status: experimental and not yet tested in game.** The plugin compiles
> against CommonLibF4RD, and its GPU pipeline passes automated tests on a real
> Direct3D 11 device. Its renderer hooks still need in-game validation. Every
> engine assumption is checked at runtime before batching turns on, and a
> failed check leaves the game rendering exactly as vanilla. See
> [docs/TESTING.md](docs/TESTING.md) for an in-game test plan.

## Why

Fallout 4's Creation Engine issues one Direct3D 11 draw call per object, per
render pass, from one CPU thread. Around 7,000-8,000 draw calls is the
practical limit. Downtown Boston and Diamond City reach 10,000-12,000. With
precombines/PreVis broken, which most world-editing mods do, dense cells pass
15,000. At that point the CPU, not the GPU, sets the frame rate. Shadow
cascades multiply the count again.

Nvidium solved the same problem in Minecraft. Terrain is kept resident on the
GPU, the GPU decides what is visible, and the GPU generates its own draw
commands. This plugin applies the same idea within Fallout 4's Direct3D 11
renderer.

## How it works

1. **Observe.** Vtable hooks on `BSLightingShaderProperty`,
   `BSLightingShader`/`BSUtilityShader` and `BSShaderAccumulator` (plus COM
   hooks on the D3D11 device, context and swap chain) watch which objects the
   engine registers for each view and which D3D11 buffers it draws them from.
2. **Merge.** Some objects are plain, static, opaque `BSTriShape`s. Those that
   share a material (the engine's own `CanMerge` precombine test), a vertex
   format and a 4096-unit grid cell are copied on the GPU into one batch. Each
   batch is stored in the local space of one member, the *anchor*. Positions
   are widened to 32-bit floats. This is a runtime precombine that never
   touches plugin files and never breaks PreVis.
3. **Route.** For each view (main camera, each shadow cascade), the first
   batch member the engine registers takes the anchor's render passes, and the
   rest report no passes. Every member registered for the view goes into that
   view's visibility list.
4. **Cull on the GPU.** The anchor's draw is reached with the engine's own
   shaders, materials and states. At that point one compute dispatch per view
   culls the listed members against a hierarchical-Z pyramid of the previous
   frame's depth. It then writes indirect draw arguments.
5. **Draw indirectly.** The anchor's `DrawIndexed` is replaced by one
   `DrawIndexedInstancedIndirect` for the whole batch, using index compaction
   (any GPU). On NVIDIA, `NvAPI_D3D11_MultiDrawIndexedInstancedIndirect` draws
   per-member records directly.

Thousands of per-object engine passes become one indirect draw per batch per
view. The objects the engine culls stay culled, and the GPU adds occlusion
culling that Fallout 4 lacks wherever PreVis is broken.

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/NVIDIUM_COMPARISON.md](docs/NVIDIUM_COMPARISON.md).

## Requirements

- Fallout 4 OG 1.10.163, NG 1.10.984 or AE 1.11.x with the matching
  [F4SE](https://f4se.silverlock.org/)
- The CommonLibF4RD runtime database: `Data/F4SE/Plugins/f4rd-runtime.bin`,
  distributed with CommonLibF4RD and not with this repository
- A Direct3D 11.0 GPU. NVIDIA drivers additionally enable the NVAPI
  multi-draw path.

## Download

The DLL is built by GitHub Actions on a Windows runner, so no Windows machine
or Visual Studio is needed to get it.

- **Releases:** tagged versions are on the
  [Releases page](https://github.com/AtomicTEM/Fallout-4-GPU-Pipeline/releases)
  as `GPUWorldPipeline-X.Y.Z.zip`, with debug symbols in
  `GPUWorldPipeline-X.Y.Z-pdb.zip` and checksums in `SHA256SUMS.txt`.
- **Development builds:** every push runs the
  [Build F4SE plugin](https://github.com/AtomicTEM/Fallout-4-GPU-Pipeline/actions/workflows/build.yml)
  workflow. Open a successful run started by a **push** and download
  `GPUWorldPipeline-<version>-<commit>` under **Artifacts** (you must be
  signed in to GitHub). The run summary also links it. Pull-request runs
  have artifacts too, including runs for pull requests from forks. They are
  builds of unreviewed code, so do not install them. From a terminal with the
  [GitHub CLI](https://cli.github.com/):

  ```sh
  repo=AtomicTEM/Fallout-4-GPU-Pipeline
  run=$(gh run list -R "$repo" -w build.yml -e push -s success -L 1 --json databaseId -q '.[0].databaseId')
  gh run download "${run:?no successful build found}" -R "$repo" -p 'GPUWorldPipeline-*'
  ```

  Add `-b <branch>` to `gh run list` to pick a branch, and keep `-e push`.
  The plugin and its PDB are downloaded into folders named after the
  artifacts.

Every package is checked on Linux by the **Verify package** job after it is
built. A run is green, and a release is published, only if that check passes,
so take artifacts from successful runs. `tools/check-package.sh` verifies the
folder layout, that the DLL is 64-bit, its F4SE exports, that it imports only
Windows system DLLs and the Visual C++ runtime, and that it embeds the
precompiled shaders. Run it on an extracted package to check it yourself. It
needs GNU binutils' `objdump`.

## Install

Both the release zip and the artifact contain `F4SE/Plugins/`. Extract them
into the game's `Data` folder, or install them with a mod manager. With Steam
on Linux (Proton), that is usually
`~/.steam/steam/steamapps/common/Fallout 4/Data`.

**Proton:** no extra components are needed. The shaders are compiled into the
DLL at build time, so Wine's built-in shader compiler is never used. The logs
are in the game's prefix:
`~/.steam/steam/steamapps/compatdata/377160/pfx/drive_c/users/steamuser/Documents/My Games/Fallout4/F4SE/`.

```text
Data/
└─ F4SE/
   └─ Plugins/
      ├─ GPUWorldPipeline.dll
      ├─ GPUWorldPipeline.ini      (optional; defaults are built in)
      └─ f4rd-runtime.bin          (CommonLibF4RD runtime database, not included)
```

The log is written to `Documents/My Games/Fallout4/F4SE/GPUWorldPipeline.log`.
**F10** toggles batching in game for A/B comparisons.

## Build

CommonLibF4RD needs MSVC, so the plugin is built on Windows. On Linux or
macOS, let GitHub Actions build it:

- Push to any branch, or open **Actions → Build F4SE plugin → Run
  workflow** to build any branch on demand. Download the result as described
  in [Download](#download). On a fork, enable Actions in the fork's settings
  and set `repo` to the fork. To wait for, and fetch, the build of the commit
  you just pushed:

  ```sh
  run=$(gh run list -R "$repo" -w build.yml -e push -c "$(git rev-parse HEAD)" -L 1 --json databaseId -q '.[0].databaseId')
  gh run watch "${run:?no run for this commit yet}" -R "$repo" --exit-status &&
    gh run download "$run" -R "$repo" -p 'GPUWorldPipeline-*'
  ```
- **To publish a release,** set `project(... VERSION X.Y.Z)` in
  `CMakeLists.txt` (and `version-string` in `vcpkg.json`), commit, then push a
  tag `vX.Y.Z`. A tag with a suffix such as `v0.2.0-beta1` gives a
  pre-release. The release is created only after the build, the package check
  and both test jobs pass. A tag that does not match the CMake version fails
  the build. To fix that, set the version, commit, and move the tag:
  `git push origin :refs/tags/vX.Y.Z && git tag -f vX.Y.Z && git push origin vX.Y.Z`.

  ```sh
  git tag v0.1.0
  git push origin v0.1.0
  ```

The workflow (`.github/workflows/build.yml`) runs these jobs:

| Job | Runner | Does |
| --- | --- | --- |
| Plugin DLL | `windows-2022` | MSVC + vcpkg (packages cached between runs) build with shaders precompiled by `fxc`, GPU tests on WARP, uploads the package and PDB |
| Verify package | `ubuntu-24.04` | `tools/check-package.sh` on the uploaded package |
| GPU tests | `ubuntu-24.04` | `tools/run-tests-wine.sh` (Wine + llvmpipe) |
| Publish release | `ubuntu-24.04` | Tags only: zips the package and PDB, writes `SHA256SUMS.txt`, creates the GitHub Release |

### Building on Windows

Requirements: Visual Studio 2022 (Desktop development with C++), CMake 3.21+,
and vcpkg with `VCPKG_ROOT` set.

```text
git clone --recursive https://github.com/AtomicTEM/Fallout-4-GPU-Pipeline.git
cd Fallout-4-GPU-Pipeline
cmake --preset vs2022-windows-vcpkg
cmake --build --preset vs2022-release
```

Output: `build/vs2022/Release/GPUWorldPipeline.dll`. Pass `-DCOPY_BUILD=ON`
with `Fallout4Path` set to copy the DLL and INI into the game folder.

The compute shaders in `shaders/` are compiled at build time by the Windows
SDK's `fxc` (`cs_5_0`) and embedded in the DLL as bytecode, so no shader
compiler is needed at runtime. Configuring fails if `fxc` is not found. Pass
`-DREQUIRE_PRECOMPILED_SHADERS=OFF` to build a DLL that compiles them at
start-up with `d3dcompiler_47.dll` instead (that does not work under Proton).
`[Debug] sShaderDirectory` loads shader files from disk and compiles them at
runtime, for iteration without rebuilding. Under Proton that needs Microsoft's
compiler in the prefix (`protontricks 377160 d3dcompiler_47`).

### Tests

`tests/GpuTests.cpp` runs the real shaders and GPU modules on a Direct3D 11
device without the game. It checks vertex/index merging, culling, compaction,
multi-draw records, Hi-Z construction and occlusion decisions.

- Windows: configure with `-DBUILD_GPU_TESTS=ON` and run `GpuTests.exe` (it
  falls back to WARP when no GPU is available).
- Linux: `tools/run-tests-wine.sh` builds with MinGW-w64 and runs under Wine
  on Mesa's llvmpipe.

CI runs both on every push (see [Build](#build)).

## Configuration

Every option is documented in
[`dist/Data/F4SE/Plugins/GPUWorldPipeline.ini`](dist/Data/F4SE/Plugins/GPUWorldPipeline.ini).
These are the ones that matter most:

| Setting | Default | Meaning |
| --- | --- | --- |
| `[General] iMode` | `1` | `0` only observes and logs; `1` batches |
| `[General] iToggleKey` | `121` (F10) | Toggles batching in game |
| `[Batching] bBatchMainView` / `bBatchShadows` | `1` / `1` | Which views are batched |
| `[Culling] bOcclusionCulling` | `1` | Previous-frame Hi-Z occlusion for the main view |
| `[Culling] iIndirectMode` | `0` | Auto, compaction, NVAPI multi-draw, or draw loop |
| `[Memory] iArenaVertexMB` / `iArenaIndexMB` | `512` / `192` | VRAM budget for merged geometry |

## What is batched, and what is not

Batched:

- Static `BSTriShape` objects that use `BSLightingShaderProperty`
- Opaque or alpha-tested objects
- Objects that stay still for 60 frames and are drawn in the main view

Not batched; these render exactly as vanilla:

- Skinned or animated geometry (actors, creatures)
- Alpha-blended geometry
- Landscape and LOD
- Effects and particles
- `BSMultiIndexTriShape`/`BSCombinedTriShape` (existing precombines)
- Objects that are fading, moving or being edited in workshop mode
- Views whose render mode is rare (local map, VATS masks, and so on)

## Credits

- [CommonLibF4RD](https://github.com/Zzyxz/CommonLibF4RD) and its
  [example plugin](https://github.com/Zzyxz/CommonLibF4RD-ExamplePlugin): the
  project template, runtime-aware relocations, and RE types
- [libxse/commonlibf4](https://github.com/libxse/commonlibf4) and
  [F4SE](https://github.com/ianpatt/f4se): engine structure layouts used as
  cross-references
- [Nvidium](https://github.com/MCRcortex/nvidium) and its
  [EXT_mesh_shader port](https://github.com/drouarb/nvidium/tree/EXT_mesh_shader):
  the design this plugin follows. No code was copied.
- The Fallout 4 performance research notes, which describe the bottleneck being
  addressed: draw-call limits, PVC trade-offs and CPU sensitivity

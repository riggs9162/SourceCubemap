# SourceCubemap

SourceCubemap is a standalone Windows C++23 tool for generating Source Engine cubemaps with Vulkan. It reads compiled BSP geometry and game assets without launching a game or loading engine binaries. The project is under development; structural validation alone does not establish rendering fidelity or in-game acceptance.

## Build

Install Visual Studio with C++ desktop tools, a Windows SDK, CMake, Ninja, and vcpkg. Set `VCPKG_ROOT` if vcpkg is installed separately. Run `tools/build.ps1` from PowerShell. The build downloads a pinned SPIR-V-capable DXC; the Windows SDK's DXC is not assumed to support SPIR-V. The release executable is written to `build/release/sourcecubemap.exe`. Subsequent builds can use `tools/build.ps1 -SkipConfigure`.

Dependencies are pinned through `vcpkg.json` and the SourcePP commit in CMake. No proprietary game assets are distributed. Supply a licensed local Source game installation.

Runtime requirements are Windows 10/11 x64 and a Vulkan 1.3 driver supporting dynamic rendering and the required floating-point texture formats. Keep the `shaders` directory beside the executable. Build a portable distribution, including dependency notices, with `tools/package.ps1 -Destination C:\Packages\SourceCubemap` using a new destination directory.

## Current commands

```powershell
sourcecubemap inspect map.bsp
sourcecubemap inspect map.bsp --game 'C:\Games\GarrysMod\garrysmod' --profile gmod
sourcecubemap validate map.bsp
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --hdr auto --output map_built.bsp
sourcecubemap benchmark map.bsp --game 'C:\Games\GarrysMod\garrysmod' --runs 5
```

Inspection reports BSP bounds, probes, lighting, embedded entries, and optionally resolved material parameters. Validation checks embedded archive integrity and VTF image layouts. Unsupported BSP versions are rejected. Asset resolution gives embedded files priority, followed by explicit mounts and game content.

The BSP writer preserves unrelated lump offsets and bytes, rebuilds the embedded ZIP, validates a temporary output, flushes it, and atomically publishes it. Input replacement requires a separate in-place option and creates a backup. Output overwrite permission never authorizes input replacement.

Automated tests and fixtures are kept locally under ignored `tests/` and `work/` directories in accordance with repository policy. Production validators remain part of the application.

## Rendering status

The headless Vulkan renderer captures actual compiled faces, primitive indices, displacements, base and directional lightmaps, basic Source materials, and six cubemap views. Capture processing produces VTF 7.4 DXT1 LDR or RGBA16161616F HDR, complete mip chains, matched face borders, and legacy spheremap data. Each encoded VTF and the final BSP are reopened and checked. The output filename does not change embedded map namespaces.

`--quality fast` uses native resolution and bilinear filtering. `standard` uses native resolution and up to 4x anisotropy. `high` captures at twice the native resolution and uses up to 16x anisotropy. `--device` selects a GPU by name. `--previews DIRECTORY` writes face PNGs, and `--json FILE` writes a report. Diagnostic logging goes to standard error. Benchmarks always regenerate captures and report measured stage durations; they never report unchanged-output reuse as rendering performance.

Basic initial transforms for selected brush entities, 3D skybox geometry and fog, conservative PVS/frustum culling, displacement blending, normal and SSBump directional lighting, alpha testing, detail blend mode zero, and translucency are implemented. Broader material and native image parity remain unverified. References to map cubemaps must match a probe or the generated default sky cubemap. Existing material patches and internal map names are preserved.

Static props now use validated VHV v2 colors when available. The supported Phong path uses ambient cubes and up to four visible compiled world lights, normal maps, exponent textures, masks, Fresnel ranges, tint controls, and Half-Lambert diffuse lighting. VHV checks include model checksums, LODs, mesh counts, vertex ranges, and VTX mapping. Phong follows the Source skin shader's ambient/direct diffuse path rather than treating VHV as its diffuse input. Authored VTF mip chains are retained. LDR lightmaps reproduce the legacy gamma/overbright conversion; HDR lightmaps retain linear radiance. Constant initial light styles and initial open-door transforms are supported.

GMod painted skies render authored gradients and a fixed-time star/cloud layer. Clouds remain opt-in through `--experimental` because their sampling does not yet match native rendering across all views. Sun discs, dusk bands, and multiple star layers fail explicitly. The sky HDR multiplier applies to the gradient; cloud radiance is unchanged between LDR and HDR, as verified against native GMod.

Strict mode rejects unsupported shaders, proxies, animated textures, animated light styles, material effects, overlays, and runtime entities. Water, planar reflections/refraction, and reflective/refractive glass remain unsupported. Experimental mode reports omitted runtime entities/overlays and the cloud approximation. It never bypasses missing dependencies or unsupported shaders. Experimental output is not visually equivalent to native buildcubemaps.

Local validation exercises malformed BSP bounds and game-lump directories, signed probes, mount order, VMT patches, missing dependencies, compressed geometry, truncated VTFs, HDR radiance above one, HDR borders, spheremap data, face orientation, unchanged lump and asset preservation, interior PAK replacement, input hardlink protection, in-place backups, and cache invalidation. An asymmetric compiled fixture and an experimental generated `gm_flatgrass` have loaded in GMod and supplied working reflective materials. The Flatgrass visual-fidelity milestone remains open. Painted-sky upward-view comparisons and a baked-prop fixture now have native comparison evidence. World-material and prop-lighting differences still need work. Construct's 364 VHV files pass decoding and model-layout checks, but rendering stops at its Water shader. `d1_trainstation_01` progresses through open-door reconstruction and stops at `func_tracktrain`. Neither map has passed rendering acceptance.

SourcePP's pinned game-lump signature constants do not match the desktop BSP's stored static-prop identifier, and its parser assumes contiguous payloads. A bounded adapter reads game-lump directory offsets and uses SourcePP's LZMA decoder. The writer retains original game-lump bytes and absolute offsets.

## Experimental Flatgrass build

```powershell
sourcecubemap build 'C:\Games\GarrysMod\garrysmod\maps\gm_flatgrass.bsp' `
  --game 'C:\Games\GarrysMod\garrysmod' --profile gmod --experimental `
  --hdr auto --quality standard --output gm_flatgrass_built.bsp --json build.json
sourcecubemap validate gm_flatgrass_built.bsp
```

This produces eight 256-pixel probes in both LDR and HDR from the installed map. Keep a copy of the report with experimental output. Renaming the output preserves `materials/maps/gm_flatgrass/` bindings. Existing destinations require `--overwrite`; replacing the input requires `--in-place` without `--output` and creates a backup.

## Caches and measurement

Persistent scene, pipeline, and encoded-capture caches default to `%LOCALAPPDATA%\SourceCubemap\cache`. Use `--cache DIRECTORY` to isolate a run or `--no-cache` to disable application cache access. Asset content, provenance, mount ordering, map bytes, lighting selection, rendering settings, shader binaries, and device identity participate in validation. Corrupt caches are rejected and regenerated. `--incremental` explicitly enables unchanged capture reuse; normal builds and benchmarks always render every requested face.

Reports separate parsing, Vulkan startup, scene/asset loading, GPU preparation, rendering, encoding, transactional writing, final validation, and total build time. Renderer statistics include GPU timestamps, draw/cull counts, uploaded bytes, upload waiting, and pipeline creation. Total time is measured inside the build operation; external process wall time must be measured separately. Preview export, cache I/O, and report construction are not all included in individual stage totals. GPU timestamps describe submitted capture work, not the entire application.

Benchmark an empty application cache separately from warmed full regeneration and incremental reuse. A disabled cache is not equivalent to a cold cache that must be populated. Do not compare simultaneous-game measurements with an idle-GPU run. No persistent worker is implemented; recorded Vulkan startup has not established the required 10% savings case. See `docs/ARCHITECTURE.md` for module contracts and remaining work.

## Licensing

Project code is MIT licensed. See `THIRD_PARTY.md` for dependency and reference boundaries. Valve SDK and GPL viewer code are reference-only and are not incorporated into this project.

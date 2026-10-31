# SourceCubemap

SourceCubemap is a standalone Windows C++23 tool for generating Source Engine cubemaps with Vulkan. It reads compiled BSP geometry and game assets without launching a game or loading engine binaries. The project is under development; structural validation alone does not establish rendering fidelity or in-game acceptance.

## Build

Install Visual Studio with C++ desktop tools, a Windows SDK, CMake, Ninja, and vcpkg. Set `VCPKG_ROOT` if vcpkg is installed separately. Run `tools/build.ps1` from PowerShell. The build downloads a pinned SPIR-V-capable DXC; the Windows SDK's DXC is not assumed to support SPIR-V. The release executable is written to `build/release/sourcecubemap.exe`. Subsequent builds can use `tools/build.ps1 -SkipConfigure`.

Dependencies are pinned through `vcpkg.json` and the SourcePP commit in CMake. No proprietary game assets are distributed. Supply a licensed local Source game installation.

Runtime requirements are Windows 10/11 x64 and a Vulkan 1.3 driver supporting dynamic rendering, BC texture compression, and the required floating-point texture formats. Keep the `shaders` directory beside the executable. Build a portable distribution, including dependency notices, with `tools/package.ps1 -Destination C:\Packages\SourceCubemap` using a new destination directory.

Prebuilt Windows packages are on the [releases page](https://github.com/riggs9162/SourceCubemap/releases). They are built on a development machine with `tools/release.ps1`, not by a hosted workflow.

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

The headless Vulkan renderer captures actual compiled faces, primitive indices, displacements, base and directional lightmaps, basic Source materials, and six cubemap views. Capture processing produces VTF 7.4 DXT1 LDR or RGBA16161616F HDR, complete mip chains, matched face borders, and legacy spheremap data. Each encoded VTF and the final BSP are reopened and checked. Compiled material namespaces stay intact. A renamed output also receives cubemap aliases for runtime static-prop lookups. Keep the generated filename when installing the BSP.

`--quality fast` uses native resolution and bilinear filtering. `standard` uses native resolution and up to 4x anisotropy. `high` captures at twice the native resolution and uses up to 16x anisotropy. `--device` selects a GPU by name. `--previews DIRECTORY` writes face PNGs, and `--json FILE` writes a report. Diagnostic logging goes to standard error. Benchmarks always regenerate captures and report measured stage durations; they never report unchanged-output reuse as rendering performance.

Basic initial transforms for selected brush entities, 3D skybox geometry and fog, conservative PVS/frustum culling, displacement blending, normal and SSBump directional lighting, alpha testing, detail blend mode zero, and translucency are implemented. Broader material and native image parity remain unverified. References to map cubemaps must match a probe or the generated default sky cubemap. Existing material patches and internal map names are preserved.

Maps packed with Valve's repacking tool are read, including compressed game lumps and LZMA archive entries. Static-prop layouts v4, v5, v6, and the 72-byte v10 layout are supported; per-texel prop lightmaps remain unsupported. Static props use validated VHV v2 colors when available. The supported Phong path uses ambient cubes and up to four visible compiled world lights, normal maps, exponent textures, masks, Fresnel ranges, tint controls, and Half-Lambert diffuse lighting. VHV checks include model checksums, LODs, mesh counts, vertex ranges, and VTX mapping. Phong follows the Source skin shader's ambient/direct diffuse path rather than treating VHV as its diffuse input. With `--profile gmod`, LightmappedGeneric and WorldVertexTransition materials also render Phong: diffuse stays baked in the lightmap and specular comes from world lights chosen per 256-unit cell. Other Source games ignore those parameters, so other profiles do too. Authored VTF mip chains are retained. LDR lightmaps reproduce the legacy gamma/overbright conversion; HDR lightmaps retain linear radiance. Constant initial light styles, initial open-door transforms, and unparented train path poses at capture time zero are supported. Sine and texture-scroll proxies evaluate at the selected capture time; entity texture toggles use their authored initial frame in experimental mode. WorldVertexTransition blend masks, UnlitTwoTexture, and detail blend modes 0-5, 8, 9, and 11 are implemented; modes 6 and 10 use the closest implemented mode.

`env_sprite`, `env_sprite_oriented`, `env_glow`, and `env_sprite_clientside` render as camera-facing or oriented quads with depth occlusion, and sprite and decal surfaces no longer stop a build. Glow falloff is approximate and has not been compared with native captures.

GMod painted skies render authored gradients and a fixed-time star/cloud layer. Clouds remain opt-in through `--experimental` because their sampling does not yet match native rendering across all views. Sun and dusk shading are available in experimental mode; dusk response remains approximate. Multiple star layers fail explicitly. The sky HDR multiplier applies to the gradient; cloud radiance is unchanged between LDR and HDR, as verified against native GMod.

Strict mode rejects unsupported shaders, proxies, animated textures, animated light styles, material effects, overlays, and runtime entities. Each refusal names `--experimental`, which continues with the closest supported behaviour and lists every approximation in the report. `--water planar --experimental` supports one horizontal water level, one reflection level, depth refraction, fog, and Fresnel effects. Secondary water views omit translucent/additive surfaces; underwater fog uses the above-water settings. Reflective glass, live monitors, smoke volumes, and other runtime effects are explicitly omitted in experimental mode. Unknown shaders and proxies are omitted with a warning in experimental mode. Experimental mode also continues past missing assets: a missing material or color texture renders flat gray, other missing textures are ignored, and a prop whose model is missing is left out. Every substitution appears in the report's warnings, and `missing_assets` counts them, so check the report before trusting that output. Strict mode stops on the first missing asset. Corrupt data stops generation in both modes. Selected SDK shader aliases are opt-in approximations. Experimental output is not visually equivalent to native buildcubemaps.

Local validation exercises malformed BSP bounds and game-lump directories, signed probes, mount order, VMT patches, missing dependencies, compressed geometry, truncated VTFs, HDR radiance above one, HDR borders, spheremap data, face orientation, unchanged lump and asset preservation, interior PAK replacement, input hardlink protection, in-place backups, and cache invalidation. An asymmetric compiled fixture and an experimental generated `gm_flatgrass` have loaded in GMod and supplied working reflective materials. The Flatgrass visual-fidelity milestone remains open. Painted-sky upward-view comparisons and a baked-prop fixture now have native comparison evidence. World-material and prop-lighting differences still need work. Construct now generates cubemaps with experimental planar water. HL2's `d1_trainstation_01` generates its 79 authored probes and default sky with initial tracktrain/path-train poses. Native visual acceptance for these maps remains incomplete. The vault example supports its v10 static props, SDK shader aliases, and separate self-illumination masks. Smoke volumes require explicit experimental omission.

SourcePP's pinned game-lump signature constants do not match the desktop BSP's stored static-prop identifier, and its parser assumes contiguous payloads. A bounded adapter reads game-lump directory offsets and uses SourcePP's LZMA decoder. The writer retains original game-lump bytes and absolute offsets.

## Windows interface

Run `sourcecubemap-gui.exe` beside the CLI executable and shaders. Choose a BSP, installed content directory, output path, mounts, lighting, quality, and water mode. Inspect the map, generate cubemaps, review face previews and warnings, then validate the output. Cancellation waits for a safe boundary; publication already in progress completes atomically. Job logs and reports are saved under `%LOCALAPPDATA%\SourceCubemap\jobs`. Settings persist between sessions. The GUI invokes the same standalone pipeline as the CLI.

For `vault_example.bsp`, enable experimental rendering to explicitly omit its smoke particles and accept the supported SDK shader aliases. The smoke volume's brush bounds are never rendered as solid geometry. Use planar water plus experimental rendering for Construct.

## Experimental Flatgrass build

```powershell
sourcecubemap build 'C:\Games\GarrysMod\garrysmod\maps\gm_flatgrass.bsp' `
  --game 'C:\Games\GarrysMod\garrysmod' --profile gmod --experimental `
  --hdr auto --quality standard --output gm_flatgrass_built.bsp --json build.json
sourcecubemap validate gm_flatgrass_built.bsp
```

This produces eight 256-pixel probes in both LDR and HDR from the installed map. Keep a copy of the report with experimental output. Renaming the output preserves `materials/maps/gm_flatgrass/` bindings. Existing destinations require `--overwrite`; replacing the input requires `--in-place` without `--output` and creates a backup.

## Caches and measurement

Cache writes default to a **2 GiB budget per selected cache directory**, retain **8 GiB of free disk space**, and skip decoded scenes larger than **512 MiB**. Set `--cache-limit-mb` to change the budget explicitly; zero disables cache writes. Existing oversized caches are read-only until cleared, and are never silently deleted. These limits apply to cache writes, not generated BSPs, previews, or external diagnostic work directories.

Texture data stays in its original compressed form, so cached scenes are far smaller than in earlier versions. Persistent scene, pipeline, and encoded-capture caches default to `%LOCALAPPDATA%\SourceCubemap\cache`. Use `--cache DIRECTORY` to isolate a run or `--no-cache` to disable application cache access. Asset content, provenance, mount ordering, map bytes, lighting selection, rendering settings, shader binaries, and device identity participate in validation. Corrupt caches are rejected and regenerated. `--incremental` explicitly enables unchanged capture reuse and skips Vulkan startup and scene loading entirely when every capture still matches; normal builds and benchmarks always render every requested face.

Reports separate parsing, Vulkan startup, scene/asset loading, GPU preparation, rendering, encoding, transactional writing, final validation, and total build time. Work overlaps across CPU threads, so stages do not add up to the total: `encoding` is time summed across worker threads, and `encoding_wait` is how long the build actually waited for them. Renderer statistics include GPU timestamps, draw/cull counts, uploaded bytes, textures reused between lighting variants, upload waiting, and pipeline creation. Total time is measured inside the build operation; external process wall time must be measured separately. Preview export, cache I/O, and report construction are not all included in individual stage totals. GPU timestamps describe submitted capture work, not the entire application.

Benchmark an empty application cache separately from warmed full regeneration and incremental reuse. A disabled cache is not equivalent to a cold cache that must be populated. Do not compare simultaneous-game measurements with an idle-GPU run. No persistent worker is implemented; recorded Vulkan startup has not established the required 10% savings case. See `docs/ARCHITECTURE.md` for module contracts and remaining work.

## Licensing

Project code is MIT licensed. See `THIRD_PARTY.md` for dependency and reference boundaries. Valve SDK and GPL viewer code are reference-only and are not incorporated into this project.

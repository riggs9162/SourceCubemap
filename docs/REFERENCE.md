# Technical reference

Detailed behavior behind the [README](../README.md). See [Architecture](ARCHITECTURE.md) for how the pipeline fits together and [Validation](VALIDATION.md) for test results.

## Requirements

- Windows 10/11 x64.
- A Vulkan 1.3 driver supporting dynamic rendering, BC texture compression, and the required floating-point texture formats.
- A licensed local Source game installation. No proprietary game assets are distributed.
- The `shaders` directory beside the executable.

## Commands

```powershell
sourcecubemap inspect map.bsp
sourcecubemap inspect map.bsp --game 'C:\Games\GarrysMod\garrysmod' --profile gmod
sourcecubemap validate map.bsp
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --hdr auto --output map_built.bsp
sourcecubemap benchmark map.bsp --game 'C:\Games\GarrysMod\garrysmod' --runs 5
```

- `inspect` reports BSP bounds, probes, lighting, embedded entries, and optionally resolved material parameters.
- `validate` checks embedded archive integrity and VTF image layouts. Unsupported BSP versions are rejected.
- `build` renders cubemaps and embeds them into a new BSP.
- `benchmark` always regenerates captures and reports measured stage durations. It never reports unchanged-output reuse as rendering performance.

Asset resolution gives embedded files priority, followed by explicit mounts and game content. `--device` selects a GPU by name. Diagnostic logging goes to standard error.

## Output safety

The BSP writer preserves unrelated lump offsets and bytes, rebuilds the embedded ZIP, validates a temporary output, flushes it, and atomically publishes it.

Existing destinations require `--overwrite`. Replacing the input requires `--in-place` without `--output` and creates a backup. `--no-backup` skips that backup for tools that keep their own copy of the map, such as a compile pipeline that can rebuild it. Output overwrite permission never authorizes input replacement.

Each encoded VTF and the final BSP are reopened and checked. Compiled material namespaces stay intact. A renamed output also receives cubemap aliases for runtime static-prop lookups, which is why the generated filename must be kept when installing the BSP. Renaming the output preserves the original `materials/maps/` bindings.

SourcePP's pinned game-lump signature constants do not match the desktop BSP's stored static-prop identifier, and its parser assumes contiguous payloads. A bounded adapter reads game-lump directory offsets and uses SourcePP's LZMA decoder. The writer retains original game-lump bytes and absolute offsets.

## Capture

Capture produces VTF 7.4 DXT1 for LDR or RGBA16161616F for HDR, complete mip chains, matched face borders, and legacy spheremap data.

| Quality | Resolution | Filtering |
| --- | --- | --- |
| `fast` | Native | Bilinear |
| `standard` | Native | Up to 4x anisotropy |
| `high` | Twice native | Up to 16x anisotropy |

References to map cubemaps must match a probe or the generated default sky cubemap. Existing material patches and internal map names are preserved. Cubemaps at the same position share one texture, so samples that request different sizes use the largest.

## Geometry and entities

- Compiled faces, primitive indices, and displacements with blending.
- 3D skybox geometry and fog, with conservative PVS and frustum culling.
- Basic initial transforms for selected brush entities, including initial open-door transforms.
- Unparented train path poses at capture time zero.
- Maps packed with Valve's repacking tool, including compressed game lumps and LZMA archive entries.

## Static props

- Layouts v4, v5, v6, and the 72-byte v10 layout are supported. Per-texel prop lightmaps are not.
- Static props use validated VHV v2 colors when available. Checks cover model checksums, LODs, mesh counts, vertex ranges, and VTX mapping.
- The Phong path uses ambient cubes and up to four visible compiled world lights, normal maps, exponent textures, masks, Fresnel ranges, tint controls, and Half-Lambert diffuse lighting.
- Phong follows the Source skin shader's ambient and direct diffuse path rather than treating VHV as its diffuse input.

## Lighting

- LDR lightmaps reproduce the legacy gamma and overbright conversion. HDR lightmaps retain linear radiance.
- Normal and SSBump directional lighting are supported.
- Constant initial light styles are supported.

## Materials

- Authored VTF mip chains are retained.
- Alpha testing, translucency, WorldVertexTransition blend masks, and UnlitTwoTexture are implemented.
- Detail blend modes 0-5, 8, 9, and 11 are implemented. Modes 6 and 10 use the closest implemented mode.
- Sine and texture-scroll proxies evaluate at the selected capture time. Entity texture toggles use their authored initial frame in experimental mode.
- With `--profile gmod`, LightmappedGeneric and WorldVertexTransition materials also render Phong: diffuse stays baked in the lightmap, and specular comes from world lights chosen per 256-unit cell. Other Source games ignore those parameters, so other profiles do too.
- Selected SDK shader aliases are opt-in approximations.

## Sprites

`env_sprite`, `env_sprite_oriented`, `env_glow`, and `env_sprite_clientside` render as camera-facing or oriented quads with depth occlusion. Sprite and decal surfaces no longer stop a build. Glow falloff is approximate and has not been compared with native captures.

## Skies

GMod painted skies render authored gradients and a fixed-time star and cloud layer. Clouds remain opt-in through `--experimental` because their sampling does not yet match native rendering across all views. Sun and dusk shading are available in experimental mode, and the dusk response remains approximate. Multiple star layers fail explicitly.

The sky HDR multiplier applies to the gradient. Cloud radiance is unchanged between LDR and HDR, as verified against native GMod.

## Water

`--water planar --experimental` supports one horizontal water level, one reflection level, depth refraction, fog, and Fresnel effects. Secondary water views omit translucent and additive surfaces. Underwater fog uses the above-water settings.

## Strict and experimental modes

Strict mode rejects unsupported shaders, proxies, animated textures, animated light styles, material effects, overlays, and runtime entities. Each refusal names `--experimental`, which continues with the closest supported behavior and lists every approximation in the report.

In experimental mode:

- Reflective glass, live monitors, smoke volumes, and other runtime effects are omitted.
- Unknown shaders and proxies are omitted with a warning.
- A missing material or color texture renders flat gray, other missing textures are ignored, and a prop whose model is missing is left out. Every substitution appears in the report's warnings, and `missing_assets` counts them.

Strict mode stops on the first missing asset. Corrupt data stops generation in both modes. Experimental output is not visually equivalent to native `buildcubemaps`.

## Map-specific notes

- `vault_example` needs experimental rendering to omit its smoke particles and accept its SDK shader aliases. The smoke volume's brush bounds are never rendered as solid geometry.
- `gm_construct` needs `--water planar --experimental`.
- `gm_flatgrass` produces eight 256-pixel probes in both LDR and HDR.

## Windows interface

`sourcecubemap-gui.exe` invokes the same standalone pipeline as the command line, so keep it beside `sourcecubemap.exe` and the shaders. It builds with `--experimental --water planar` unless `Options...` turns those off, uses the `gmod` profile when the game folder is named `garrysmod` and `hl2` otherwise, and asks before replacing an existing output. Capture time, GPU selection, and cache settings are command-line only. Cancellation waits for a safe boundary, and publication already in progress completes atomically. Each job's report, previews, and log are saved under `%LOCALAPPDATA%\SourceCubemap\jobs`, and the log ends with every approximation the build made. Settings persist between sessions.

## Caches

Persistent scene, pipeline, and encoded-capture caches default to `%LOCALAPPDATA%\SourceCubemap\cache`. Use `--cache DIRECTORY` to isolate a run or `--no-cache` to disable application cache access.

- Cache writes default to a 2 GiB budget per cache directory, retain 8 GiB of free disk space, and skip scenes larger than 512 MiB.
- Set `--cache-limit-mb` to change the budget. Zero disables cache writes.
- Existing oversized caches are read-only until cleared and are never silently deleted.
- These limits apply to cache writes, not generated BSPs, previews, or external diagnostic work directories.
- Asset content, provenance, mount ordering, map bytes, lighting selection, rendering settings, shader binaries, and device identity participate in validation. Corrupt caches are rejected and regenerated.

`--incremental` explicitly enables unchanged capture reuse, and skips Vulkan startup and scene loading entirely when every capture still matches. Normal builds and benchmarks always render every requested face.

## Reports

Reports separate parsing, Vulkan startup, scene and asset loading, GPU preparation, rendering, encoding, transactional writing, final validation, and total build time.

Work overlaps across CPU threads, so stages do not add up to the total. `encoding` is time summed across worker threads, and `encoding_wait` is how long the build actually waited for them.

Renderer statistics include GPU timestamps, draw and cull counts, uploaded bytes, textures reused between lighting variants, upload waiting, and pipeline creation. Total time is measured inside the build operation, so external process wall time must be measured separately. Preview export, cache I/O, and report construction are not all included in individual stage totals. GPU timestamps describe submitted capture work, not the entire application.

## Measuring performance

Benchmark an empty application cache separately from warmed full regeneration and incremental reuse. A disabled cache is not equivalent to a cold cache that must be populated. Do not compare simultaneous-game measurements with an idle-GPU run. No persistent worker is implemented, because recorded Vulkan startup has not established the required 10% savings case.

## Building and packaging

The build needs Visual Studio with C++ desktop tools, a Windows SDK, CMake, Ninja, and vcpkg. Set `VCPKG_ROOT` if vcpkg is installed separately. The build downloads a pinned SPIR-V-capable DXC, because the Windows SDK's DXC is not assumed to support SPIR-V. Dependencies are pinned through `vcpkg.json` and the SourcePP commit in CMake.

Build a portable distribution, including dependency notices, with `tools/package.ps1 -Destination C:\Packages\SourceCubemap` using a new destination directory. Published releases are built on a development machine with `tools/release.ps1`, not by a hosted workflow.

## Testing

Automated tests and fixtures are kept locally under ignored `tests/` and `work/` directories in accordance with repository policy. Production validators remain part of the application.

Local validation exercises malformed BSP bounds and game-lump directories, signed probes, mount order, VMT patches, missing dependencies, compressed geometry, truncated VTFs, HDR radiance above one, HDR borders, spheremap data, face orientation, unchanged lump and asset preservation, interior PAK replacement, input hardlink protection, in-place backups, and cache invalidation.

An asymmetric compiled fixture and an experimental generated `gm_flatgrass` have loaded in GMod and supplied working reflective materials. Painted-sky upward-view comparisons and a baked-prop fixture have native comparison evidence. The Flatgrass visual-fidelity milestone remains open, and world-material and prop-lighting differences still need work. Construct generates cubemaps with experimental planar water. HL2's `d1_trainstation_01` generates its 79 authored probes and default sky with initial tracktrain and path-train poses. Native visual acceptance for these maps remains incomplete.

## Licensing boundaries

Valve SDK and GPL viewer code are reference-only and are not incorporated into this project.

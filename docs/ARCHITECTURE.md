# Implementation boundaries

SourceCubemap 0.3.0 is an experimental standalone rendering pipeline, not a complete replacement for every Source branch's buildcubemaps behavior. Production code is C++23. No engine process, engine DLL, VMF decompilation, or Python runtime participates in generation.

## Modules

| Library | Responsibility | Vulkan dependency |
| --- | --- | --- |
| `scm_core` | Bounded BSP envelopes, SourcePP parsers, ordered mounts, VMT patches, VTF validation, transactional PAK replacement | None |
| `scm_scene` | Immutable CPU geometry, decoded images, materials, lightmap atlases, VHV colors, world-light selection, models, visibility masks, persistent scene serialization | None |
| `scm_capture` | Float cubemap filtering, borders, mipmaps, fallback spheremap, SourcePP VTF encoding and preview export | None |
| `scm_renderer` | Headless Vulkan device, resource uploads, pipelines, scene submission, six-face floating-point readback | Vulkan 1.3 |
| `sourcecubemap` | CLI settings, material binding checks, cache policy, timing reports, output orchestration | Via renderer |

The renderer consumes a const scene and returns CPU floating-point faces. Encoding and BSP writing can be exercised without creating a Vulkan instance. BSP and VTF format support comes from SourcePP; bounded adapters cover its missing or mismatched structures. No Valve or GPL viewer implementation is incorporated.

## Capture and publication

1. Validate BSP bounds, compression envelopes, supported versions, probe sizes, output aliases, and namespaces.
2. Resolve assets in deterministic order. Embedded assets precede explicit mounts. HL2 gameinfo search entries retain their declared order; GMod applies its profile's addons/base-content precedence and mount.cfg additions. Wildcard mounts, directory assets, VPK, and GMA readers are supported.
3. Resolve material patches and validate map-specific cubemap bindings. A missing bound probe prevents output. Required defaults are generated from resolved sky materials at 32 pixels.
4. Reconstruct faces, compiled primitive indices, displacements, supported brush entities, lightmaps, supported static models, sky geometry, and conservative visibility masks.
5. Upload authored texture mip chains and lightmap atlases in bounded batches. Linear half-float images reduce transfer/storage cost; images exceeding the half-float range retain float32 precision. Keep device resources and pipelines alive across the build's probes and lighting variants.
6. Render six D3D/Source-oriented 90-degree views, with background sky, 3D skybox, world depth, and translucent passes. Probe-driven reflection feedback and automatic exposure are suppressed. Opt-in planar water adds six reflected and six refracted secondary views per probe, with one reflection level and GPU-resident intermediate textures.
7. Produce full mip chains and legacy seven-face VTF 7.4 files. LDR uses DXT1; HDR uses RGBA16161616F. HDR borders are matched before encoding. DXT1 block quantization can introduce additional border error and needs further native seam comparisons.
8. Rebuild the active ZIP without duplicate names. Replace a terminal PAK or append a new PAK without moving other lumps. Preserve unrelated lump metadata, payloads, and embedded asset contents.
9. Reopen the temporary BSP, validate VTF byte ranges, compare preserved data, flush, and atomically publish. Input replacement creates a backup first. Reopen the published output again.

Per-draw uniform buffers hold material, painted-sky, ambient, and world-light controls independently of the camera push constants. Scene cache schema 16 includes authored mip counts, sky parameters, VHV-derived vertex colors, and prop-light records. Scene caches hash their binary payloads and validate current dependencies. Capture caches additionally include shader and renderer revisions, quality, and device information. Incremental reuse is an explicit mode. Benchmark commands do not enable capture reuse.

## Current limits

- Desktop BSP v20 only; face/leaf version one and explicitly checked record layouts. Outer-compressed game-lump directories and other Source branch layouts are rejected.
- Static props support MDL/VTX/VVD with companion checksum checks and static-prop layouts v4-v6. VHV v2 BGRA vertex colors are mapped to VTX strip groups and decoded to linear lighting. Phong uses leaf ambient cubes and up to four selected world lights. Occlusion selection traces compiled solid BSP brushes; displacement and prop occlusion for this fallback path are not reproduced. Surface emitters use their direction at the lighting origin. VHV retains baked diffuse shadows for non-Phong materials. Multiple bodygroup models, branch-specific prop layouts, full native light selection, and broader visual acceptance remain unfinished.
- Procedural GMod painted skies support authored gradient colors/fade, HDR gradient scale, one cloud/star layer, and fixed-time UV scrolling. Cloud projection/filtering remains experimental. Native measurements establish that HDR does not scale cloud radiance. Sun and dusk shading are experimental; the dusk angular response is approximate. Multiple layers and unsupported appearance-changing proxies fail explicitly.
- Multiple water heights, moving/sloped/skybox water, reflective/refractive glass, most detail blend modes, animated light styles, advanced blending, and several material parameters fail explicitly. SSBump directional weighting is implemented but lacks a dedicated native comparison fixture.
- Overlays and runtime-dependent entity appearances fail in strict mode. Experimental mode lists their omission. Gameplay, Lua simulation, particles, ropes, model animation, and entity I/O simulation are outside the current renderer.
- Visibility is conservative. Unreferenced displacement faces remain visible; 3D sky displacements use BSP area classification. Props are expanded into geometry rather than instanced. Transparency is ordered by draw bounds, not per triangle.
- Model/texture decoding and capture encoding are primarily sequential. Timeline synchronization, parallel asset tasks, batched multi-probe readback, and persistent workers remain future work. A native Windows GUI now invokes the CLI worker, tracks progress, supports cancellation, and previews captured faces.
- Steam discovery is linked but not exposed as automatic game selection. Steam depot toggles, Workshop activation state, language packs, runtime addon mounting, and arbitrary gameinfo directives are not fully reproduced. Use ordered explicit mounts and dependency tracing for these cases.
- Missing dependencies stop output with their requester and searched locations. The current failure reports the first blocking dependency rather than a complete missing-asset inventory.

## Acceptance

Structural success proves bounds, serialization, and preservation checks only. Native acceptance is recorded separately. The CLI never marks a game acceptance pass merely because it wrote a BSP.

Local ignored tests include an asymmetric room compiled with official map tools, malformed file cases, archive preservation, file aliases, cache corruption/invalidation, signed probes, default sky bindings, overbright HDR, cube orientation, and HDR border comparisons. The fixture's cubemap was loaded and displayed on a reflective sphere by GMod. An experimental Flatgrass output also loaded and supplied a working reflection. Native upward-view sky comparisons now measure the gradient, cloud-only layer, and composite independently in LDR/HDR. A VHV-only prop fixture and Phong fixture exercise baked shadows and direct/ambient lighting separately. Lighting and cloud sampling still differ from native rendering, so the fidelity milestone is incomplete.

Required next acceptance targets are Flatgrass visual parity, then Construct's water/props/materials, then HL2. Each needs matched native settings, image comparisons, in-game loading, and separate empty-cache, warm-regeneration, and incremental measurements. Local tests and game-derived captures remain ignored and are not published as repository fixtures.

## Storage bounds

Cache publication uses a directory-specific Windows mutex and checks the configured byte budget, replacement size, and available disk space before writing a temporary file. The default budget is 2 GiB and the free-space reserve is 8 GiB. Decoded scene payloads over 512 MiB are not serialized. Existing larger scene caches can be memory-mapped, with an 8 GiB read limit and bounded metadata. Cache limits do not delete existing data or limit separately requested previews/BSP outputs.

Embedded ZIP entries are read and CRC-checked once through the pinned minizip-ng library. The writer handles empty entries, rebuilds unique paths, and verifies preservation before publication. Renamed maps receive runtime cubemap aliases while compiled material bindings retain their original namespace; build metadata records both namespaces for subsequent regeneration.

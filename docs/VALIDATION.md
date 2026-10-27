# Local acceptance record

Recorded on 2026-10-28 using the installed Windows x64 GMod and HL2 assets. These observations describe this machine and these inputs, not universal Source compatibility.

## Structural and rendering checks

The standalone executable built successfully with MSVC, CMake, Ninja, pinned vcpkg packages, pinned SourcePP, and pinned DXC. Its static executable imports Windows system libraries; Vulkan is loaded from the graphics driver. Generation does not launch a game or load engine binaries.

Seventeen local automated cases passed. They cover malformed BSP/game-lump bounds, compressed geometry, signed/default-size probes, declared and explicit mount precedence, VMT patches and DX9 blocks, missing dependencies, input hardlinks, JSON report aliases, in-place backups, interior PAK preservation, unrelated asset/lump bytes, VTF truncation, capture orientation, default sky references, HDR radiance of 4.0, HDR edge/corner agreement, fallback spheremap content, and dependency/corruption cache invalidation. Tests and compiled fixtures remain locally ignored.

Original `gm_flatgrass`, `gm_construct`, and `d1_trainstation_01` passed the production structural validator. A successful validation does not prove complete reconstruction or image parity.

An asymmetric compiled room rendered six expected colors and a deliberately offset marker. Its generated VTF was loaded by GMod and used on a reflective sphere. A copied experimental Flatgrass output also loaded successfully. Final separate GMod sessions confirmed:

- LDR: `mat_hdr_level=0`, generated `maps/gm_flatgrass/c0_96_-12032`, width/height 256, no material or texture error, visible reflected geometry.
- HDR: `mat_hdr_level=2`, automatic selection of generated `maps/gm_flatgrass/c0_96_-12032.hdr`, width/height 256, no material or texture error, visible reflected geometry.

Those sessions used copied maps and a temporary acceptance harness. The owned GMod processes were stopped and temporary maps, graph files, materials, captures, and harness were removed after copying evidence. Original map files were not replaced. Native loading and reflection use passed; visual parity did not. The painted sky and static-prop lighting remain experimental. Native Construct/HL2 rendering acceptance has not been completed.

## Performance measurements

Hardware: AMD Ryzen 9 7900, 12 cores/24 threads; NVIDIA GeForce RTX 3050, 8 GB; NVIDIA driver 616.56. Standard quality, eight native 256-pixel probes, both LDR/HDR, 16 generated VTF textures per complete run. The GPU was shared with the user's running CS2 session. Cold-run samples reported 99% GPU utilization before generation and approximately 6.5 GiB already allocated. The benchmark source revision is `e69c342`. Subsequent changes add validation, report-path protection, VMT fallback parsing, and packaging; the capture renderer and Flatgrass shading remain unchanged.

| Mode | Runs | Median build time | p95 build time | Capture work |
| --- | ---: | ---: | ---: | --- |
| Empty application cache | 3 | 19.72 s | 21.23 s | All 16 captures rendered |
| Warm full regeneration | 3 | 12.76 s | 13.78 s | All 16 captures rendered |
| Incremental unchanged content | 3 | 4.06 s | 4.75 s | 16 cached captures reused; zero rendered |

The below-ten-second warm full regeneration target was not achieved in these runs. Incremental reuse must not be presented as generation speed. Three observations provide only a small sample; the reported nearest-rank p95 is the maximum observed run. Cold means a new application-cache directory, not cleared OS filesystem caches or a cleared driver shader cache. External cold process wall times were 19.85, 16.39, and 21.37 seconds, including CLI/report overhead.

Warm full-regeneration stage medians were approximately 2.11 seconds for scene/asset loading, 4.27 for GPU preparation, 2.91 for rendering/readback, 1.57 for encoding, 0.92 for transactional writing, and 0.39 for final validation. Stage medians are independently calculated and do not sum exactly to total median time. Pipeline creation was below one millisecond with the warm cache. GPU preparation used two upload submissions and 260,046,880 total texture bytes across both lighting variants.

Vulkan startup occupied approximately 1.1-2.4% of warm build time. That does not justify adding a persistent worker under the plan's 10% criterion. Upload batching and half-float texture storage reduced transfer work. Measurements also exposed and removed per-texel error-message formatting overhead. The changing external GPU load prevents attributing a precise end-to-end speedup to an isolated change.

Full machine-readable build/benchmark reports and native reflection images are supplied as local delivery artifacts. They are not committed with game-derived fixtures. Further acceptance requires idle-GPU repetitions, matched native image comparisons, water/prop fidelity, and target-game coverage.

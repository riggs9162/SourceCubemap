# Local acceptance record

Recorded on 2026-10-28 for SourceCubemap 0.2.0 using installed Windows x64 GMod and HL2 assets. The tested executable was built from `96946ae`. These results describe this machine and these inputs, not universal Source compatibility.

## Structural and rendering checks

The native C++23 executable builds with MSVC, CMake, Ninja, pinned vcpkg packages, pinned SourcePP, and pinned DXC. Vulkan is loaded from the graphics driver. Generation does not launch a game, load engine binaries, or decompile a BSP.

Twenty-four local automated cases pass. Coverage includes malformed BSP/game-lump bounds, compressed geometry, signed/default-size probes, declared and explicit mount precedence, VMT patches, DX9/HDR fallback blocks, malformed numeric parameters, missing dependencies, input hardlinks, JSON report aliases, in-place backups, interior PAK preservation, unrelated asset/lump bytes, VTF truncation, capture orientation, default sky references, sky classification without visibility, HDR radiance of 4.0, HDR edge/corner agreement, fallback spheremap content, dependency/corruption cache invalidation, authored material mip chains and their cache roundtrip, initial light styles, VHV/Phong output, and initial sliding/rotating door poses. Tests and fixtures remain locally ignored.

The production structural validator accepts the original Flatgrass, Construct, and HL2 train-station maps. Final Flatgrass output contains eight 256-pixel probes in LDR and HDR, with 16 replacement VTF files. The transactional writer verifies unrelated lump bytes/offsets and embedded assets, then reopens and validates the published BSP. A structural pass does not establish complete reconstruction or visual parity.

## Native sky and prop comparisons

Separate GMod sessions capture the procedural painted sky at fixed time zero, with tone mapping scale one and post-processing disabled for the capture view. Native RenderView images are aligned to the D3D/VTF cubemap basis before comparison. The measurements below cover the upward 256-pixel view, using displayed RGB8 values; they are not full-map or linear-HDR error measurements.

| Sky case | LDR mean absolute error / 255 | HDR mean absolute error / 255 |
| --- | ---: | ---: |
| Authored Flatgrass composite | 0.44 | 0.66 |
| Isolated gradient | 0.32 | 0.26 |
| Isolated cloud layer | 4.16 | 4.16 |

The composite's 95th-percentile channel errors are 2 in LDR and 3 in HDR, with maximum 13. The cloud-only case has 95th-percentile error 17 and maximum 48. These differences keep cloud sampling experimental. Native tests show that the sky HDR multiplier scales the gradient but leaves cloud radiance unchanged. Sun discs, dusk bands, and multiple cloud/star layers remain unsupported.

A neutral compiled room exposed missing LDR lightmap transfer behavior. Applying legacy gamma/overbright encoding and sRGB decoding reduced five unobstructed face comparisons from approximately 4.4 to 0.4 RGB8 levels of mean error. The sixth native face contains an unrelated player-weapon light and is not used as a diffuse-light acceptance metric.

A compiled static-prop fixture separately exercises VHV-only and Phong materials. VHV shadow placement and the lit/dark regions agree with native captures. Edge filtering and lighting differences remain; this is not a broad prop-fidelity certification. Flatgrass's own doll has no embedded VHV and uses the ambient/world-light path. Close room views still show material/luminance differences and some prop differences. Full native Flatgrass parity remains open.

All 364 LDR/HDR VHV files from Construct's 182 static props pass header, model-checksum, LOD, vertex-count, and VTX strip-group checks. The decoder rejects 3,276 corrupted variants. The VHV fixture additionally passes 18 corrupt-input cases. This establishes parsing/mapping coverage, not Construct rendering acceptance.

## Native generated-texture acceptance

The generated Flatgrass copy loaded in separate GMod sessions with `mat_hdr_level=0` and `mat_hdr_level=2`. Native materials selected `maps/gm_flatgrass/c0_96_-12032` and `maps/gm_flatgrass/c0_96_-12032.hdr` respectively, both 256 by 256 with no material/texture error. Both produced visible environment reflections on a sphere. LDR/HDR acceptance records and screenshots accompany the local delivery. The generator itself never starts those sessions. Native texture loading, reflection use, and structural preservation are separate from image parity.

The original installed maps are not replaced. Owned acceptance processes were stopped. Temporary map copies and the Lua startup harness were removed after matching their hashes to saved copies. Automatic approval review rejected both bulk cleanup and the narrower verified-file cleanup without a specific reason. Temporary materials, native capture folders, and four generated graph files remain under their unique `scm_*20261028` / `sourcecubemap_*20261028` names. Their locations are recorded in the delivery cleanup report. They are not used by normal maps.

## Current Construct and HL2 boundaries

Construct reaches `gm_construct/water_13_beneath` and fails explicitly because the Water shader is not implemented. Initial constant light styles and its supported VHV layouts are now handled. No incomplete Construct BSP is published.

HL2 `d1_trainstation_01` progresses through initial open-door reconstruction and fails at `func_tracktrain`, whose startup path positioning is not implemented. Runtime models, scripted behavior, and other unsupported features are not certified by these checks. No incomplete HL2 BSP is published.

## Measured performance

Hardware: AMD Ryzen 9 7900, 12 cores/24 threads; NVIDIA GeForce RTX 3050, 8 GB; NVIDIA driver 616.56. Standard quality, eight native 256-pixel probes, both LDR/HDR, capture time zero. No GMod, HL2, or CS2 process ran during these measurements. GPU utilization/memory samples before each run are retained in the JSON report.

| Mode | Runs | Median build time | p95 build time | Capture work |
| --- | ---: | ---: | ---: | --- |
| Empty application cache | 3 | 9.57 s | 10.41 s | All 16 captures rendered |
| Warm full regeneration | 5 | 6.50 s | 6.54 s | All 16 captures rendered |
| Incremental unchanged content | 3 | 3.37 s | 3.52 s | 16 cached captures reused; zero rendered |

The warm full-regeneration target is met for this map and machine. This is a small sample, not a guarantee for other maps. Nearest-rank p95 is the largest observed run at these sample counts. Empty application cache does not clear Windows filesystem caches or driver caches. The timing excludes process/report overhead; external process wall times are recorded separately.

Warm full-regeneration stage medians: parsing 0.296 s; Vulkan startup 0.195 s; scene/asset loading 1.792 s; GPU preparation 1.404 s; rendering/readback 0.119 s; encoding 1.402 s; transactional writing 0.767 s; final validation 0.445 s. Independently calculated stage medians do not sum exactly to the total median. Texture upload totals 339,039,584 bytes across both variants in four submissions. Authored mip levels are uploaded directly.

Vulkan startup is approximately 3% of the measured warm build. This does not justify a persistent worker under the plan's 10% threshold. The earlier 0.1.0 record measured 12.76 s warm median while CS2 shared the GPU; that workload differs, so it is not a controlled speedup comparison.

Machine-readable reports, native captures, and the generated local BSP accompany the delivery. No game-derived fixtures or captures are committed to the repository.

## Version 0.3.0 continuation

The native Vulkan pipeline passed 33 local automated tests before storage limits were added. Added coverage includes smoke-volume omission, train path initialization, v10 static-prop bounds, malformed texture transforms, ZIP CRCs and empty entries, repeated namespace-preserving regeneration, cancellation, cached previews, and real GPU water reflection/refraction/fog pixels. A separate focused test then passed cache-budget enforcement, replacement accounting, disabled writes, and preservation without loading games or map assets.

Real builds completed for vault_example (12 HDR probes plus default sky), gm_construct (28 probes in both variants), gm_flatgrass (8 probes in both variants), and d1_trainstation_01 (79 probes plus default sky in both variants). All outputs passed structural and unrelated-data preservation checks. Output-name aliases increase the physical VTF entry count. GMod loaded the generated vault BSP and resolved both original and renamed-map HDR cubemaps at 32 pixels without texture/material errors; both produced actual reflection captures. Smoke particles remain omitted with an explicit experimental warning.

Directional lightmap normalization reduced the near-prop HDR six-face RGB mean absolute errors from 11.79/10.94/10.36/9.39/7.16/16.59 to 4.23/5.69/3.50/3.77/4.88/5.29 on a 0-255 scale in the local matched comparison. Flatgrass full visual parity remains incomplete, particularly cloud projection and prop shading. Construct water and HL2 runtime appearance are not native-certified.

The GUI was exercised through inspection, actual fixture generation, face previews, validation, and refusal to overwrite an existing output. Its final layout was visually inspected.

Version 0.3 measurements before the storage-limit change include a Flatgrass regeneration median of 5.295 seconds and p95 of 8.482 seconds over three runs. A user-started GMod session overlapped these measurements; they are shared-GPU observations, not isolated performance acceptance. Other reports include cold-cache writes and rejected oversized cache loads, so they must not be described as uniformly warm-cache results. Those reports are retained separately. The final storage limits were not re-benchmarked.

At approximately 13:57 local time on 2026-10-28, Windows reported an AHCI controller reset, unexpected removal of disk 1, and an NTFS delayed metadata write failure. S: became unavailable, preventing further HL2 native acceptance. Generation and benchmarking were stopped. Read-only evidence does not establish which workload caused the drive failure. No drive repair, reinitialization, remount, or reboot was attempted in this continuation.

About 42 GiB of generated cache/test output was identified for cleanup after the user requested it. Automatic approval review rejected deletion even after explicit confirmation. A reviewed PowerShell cleanup script was supplied for the user to run; deletion has not been claimed as completed. Source, executable builds, test scripts, and delivered BSPs are excluded.

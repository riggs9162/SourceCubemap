# SourceCubemap

SourceCubemap is a standalone Windows C++23 tool for generating Source Engine cubemaps with Vulkan. It reads compiled BSP geometry and game assets without launching a game or loading engine binaries. The project is under development; structural validation alone does not establish rendering fidelity or in-game acceptance.

## Build

Install Visual Studio with C++ desktop tools, CMake, Ninja, vcpkg, and a Windows SDK containing DXC with SPIR-V support. Set `VCPKG_ROOT` if vcpkg is installed separately. Run `tools/build.ps1` from PowerShell. The release executable is written to `build/release/sourcecubemap.exe`.

Dependencies are pinned through `vcpkg.json` and the SourcePP commit in CMake. No proprietary game assets are distributed. Supply a licensed local Source game installation.

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

Strict mode rejects unsupported shaders, proxies, animated textures, light styles, brush entities, and unverified static-prop lighting. Water and procedural painted-sky rendering are not implemented. `--experimental` explicitly permits the installed fallback painted-sky textures, ambient-cube model lighting, and omitted Phong specular. Reports enumerate these approximations. Experimental output is not visually equivalent to native buildcubemaps.

Local validation has exercised malformed BSP bounds, signed probes, mount order, VMT patches, missing dependencies, HDR encoding, face orientation, unchanged lump and asset preservation, interior PAK replacement, input hardlink protection, and in-place backups. An asymmetric compiled fixture has also been loaded by GMod, where its generated cubemap rendered correctly on a reflective sphere. This does not establish acceptance for gm_flatgrass, gm_construct, or HL2 maps.

SourcePP's pinned game-lump signature constants do not match the desktop BSP's stored static-prop identifier, and its parser assumes contiguous payloads. A bounded adapter reads game-lump directory offsets and uses SourcePP's LZMA decoder. The writer retains original game-lump bytes and absolute offsets.

## Licensing

Project code is MIT licensed. See `THIRD_PARTY.md` for dependency and reference boundaries. Valve SDK and GPL viewer code are reference-only and are not incorporated into this project.

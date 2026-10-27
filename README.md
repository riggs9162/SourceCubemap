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
```

Inspection reports BSP bounds, probes, lighting, embedded entries, and optionally resolved material parameters. Validation checks embedded archive integrity and VTF image layouts. Unsupported BSP versions are rejected. Asset resolution gives embedded files priority, followed by explicit mounts and game content.

The BSP writer preserves unrelated lump offsets and bytes, rebuilds the embedded ZIP, validates a temporary output, flushes it, and atomically publishes it. Input replacement requires a separate in-place option and creates a backup. Output overwrite permission never authorizes input replacement.

Automated tests and fixtures are kept locally under ignored `tests/` and `work/` directories in accordance with repository policy. Production validators remain part of the application.

## Licensing

Project code is MIT licensed. See `THIRD_PARTY.md` for dependency and reference boundaries. Valve SDK and GPL viewer code are reference-only and are not incorporated into this project.

# SourceCubemap

English | [Deutsch](docs/translations/README.de.md) | [Español](docs/translations/README.es.md) | [Français](docs/translations/README.fr.md) | [Português (Brasil)](docs/translations/README.pt-BR.md) | [Русский](docs/translations/README.ru.md) | [简体中文](docs/translations/README.zh-CN.md)

Generate cubemaps for Source Engine maps in seconds, without launching the game.

SourceCubemap reads a compiled `.bsp` and your installed game content, renders every cubemap on your graphics card, and writes a new `.bsp` with the cubemaps built in. Your original map is left alone.

| Map | Cubemaps | Time |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |

Measured on a Ryzen 9 7900 and RTX 3050.

> **Experimental:** reflections look close to the real thing, but they are not pixel-identical to the game's `buildcubemaps` command. Test your map in-game before shipping it.

## Quick start

You need 64-bit Windows 10 or 11, a graphics driver with Vulkan 1.3, and a Source game installed.

1. Download the latest zip from the [releases page](https://github.com/riggs9162/SourceCubemap/releases) and unzip it anywhere. Keep the files together.
2. Run `sourcecubemap-gui.exe`.
3. Choose your `.bsp` and your game's content folder, such as `GarrysMod\garrysmod`.
4. Turn on experimental rendering, which most custom maps need.
5. Generate, check the face previews and warnings, then validate the result.
6. Copy the new `.bsp` into your game's `maps` folder. **Keep its generated filename**, because the cubemaps are stored under it.

## Command line

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| Option | What it does |
| --- | --- |
| `--game FOLDER` | Your game's content folder. Required. |
| `--profile gmod` or `hl2` | Which game's rules to follow. Defaults to `gmod`. |
| `--experimental` | Keep going on anything that can't be reproduced exactly, and list it in the report. |
| `--water planar` | Render water reflections. Needs `--experimental`. |
| `--hdr auto`, `ldr`, `hdr`, or `both` | Which lighting to generate. `auto` builds whatever the map has. |
| `--quality fast`, `standard`, or `high` | Trade speed for texture sharpness. `high` renders at double resolution. |
| `--incremental` | Reuse cubemaps that haven't changed. |
| `--mount PATH` | Add extra content folders or archives. |
| `--previews FOLDER` | Save each cubemap face as a PNG. |
| `--json FILE` | Save a full report, including every warning. |
| `--output FILE` | Where to write the map. Defaults to `MAP_built.bsp`. |
| `--overwrite` | Replace an existing output file. |
| `--in-place` | Replace the original map, after making a backup. |

Run `sourcecubemap build --help` for the full list. `sourcecubemap inspect map.bsp` shows what a map contains without building anything.

## When a build stops

By default, SourceCubemap stops if a map uses something it can't reproduce, and tells you what it was. Add `--experimental` to continue with the closest match instead.

With `--experimental`, a missing material or color texture renders flat gray and a prop with a missing model is left out. Every substitution is listed in the report, so check it before trusting the result. If you see many missing assets, mount the content the map needs with `--mount`.

A map with no cubemap samples has nothing to generate, so place `env_cubemap` entities and recompile it first. Corrupt files always stop the build.

## What it renders

- World geometry, displacements, brush entities, and static props
- Lightmaps, in both LDR and HDR
- 3D skyboxes, fog, and Garry's Mod painted skies
- Common Source materials, including Phong and detail textures
- Planar water, sprites, and glows
- Maps compressed with Valve's repacking tool

Only desktop BSP version 20 maps are supported. Gameplay, Lua, particles, ropes, and animated models are not rendered. See the [technical reference](docs/REFERENCE.md) for exact coverage.

## Build from source

Install Visual Studio with the C++ desktop workload, then run this from PowerShell:

```powershell
tools/build.ps1
```

The program is written to `build/release`. Use `tools/build.ps1 -SkipConfigure` for later builds. Dependencies are pinned, and no game assets are included.

## Documentation

- [Technical reference](docs/REFERENCE.md): rendering coverage, caches, and reports
- [Architecture](docs/ARCHITECTURE.md): how the pipeline fits together, and its limits
- [Validation](docs/VALIDATION.md): test results and measurements

Releases are numbered `A.B.cc`: `A` for breaking changes, `B` for backwards-compatible changes, and `cc` for fixes.

## License

MIT. See [THIRD_PARTY.md](THIRD_PARTY.md) for dependencies and references.

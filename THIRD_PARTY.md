# Dependencies and references

SourceCubemap's own code is licensed under MIT. Build dependencies retain their upstream licenses. SourcePP is pinned to `1540277f99bacee288f6786a66ee182aab6c146b`; vcpkg packages are pinned through the manifest baseline.

Direct dependencies include SourcePP (MIT), Vulkan headers (Apache-2.0), Volk (MIT), Vulkan Memory Allocator (MIT), GLM (MIT), CLI11 (BSD-3-Clause), spdlog (MIT), nlohmann/json (MIT), and xxHash (BSD-2-Clause). SourcePP additionally builds archive, compression, texture conversion, and model support libraries with their own notices. Distribution packages must retain the licenses of linked dependencies, including transitive dependencies; source downloads and vcpkg package directories contain their authoritative notices.

Technical references:

- [Valve Source SDK 2013](https://github.com/ValveSoftware/source-sdk-2013): BSP definitions, public VTF interface, cubemap naming and defaults, view rendering, lightmap conventions, and Source shader behavior. Its SDK license does not license this application's code. No Valve implementation is copied into the project.
- [SourcePP](https://github.com/craftablescience/sourcepp): reused parsers and texture/archive serializers under MIT.
- [Lumper](https://github.com/momentum-mod/lumper): BSP offset and compressed-lump handling reference under MIT.
- [bsp-to-glb](https://github.com/Hona/bsp-to-glb): compiled geometry and displacement reconstruction reference under MIT.
- [bsp_viewer](https://github.com/yunusemregul/bsp_viewer): reference-only review under GPL-3.0; no implementation is incorporated.

The public SDK does not include the complete engine `buildcubemaps` implementation. This project does not use leaked source, proprietary engine binaries, or redistribute installed game assets.

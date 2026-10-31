# SourceCubemap

*Esta es una traducción. La versión más actualizada es siempre el [README en inglés](../../README.md).*

[English](../../README.md) | [Deutsch](README.de.md) | Español | [Français](README.fr.md) | [Português (Brasil)](README.pt-BR.md) | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

Genera cubemaps para mapas de Source Engine en segundos, sin abrir el juego.

SourceCubemap lee un `.bsp` compilado y el contenido de tus juegos instalados, renderiza todos los cubemaps en tu tarjeta gráfica y escribe un `.bsp` nuevo con los cubemaps incluidos. Tu mapa original no se toca.

| Mapa | Cubemaps | Tiempo |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |
| Half-Life 2, 76 mapas | 6167 | 77 s en total |

Medido con un Ryzen 9 7900 y una RTX 3050.

> **Experimental:** los reflejos se acercan mucho al resultado real, pero no son idénticos píxel a píxel a los del comando `buildcubemaps` del juego. Prueba tu mapa dentro del juego antes de publicarlo.

## Inicio rápido

Necesitas Windows 10 u 11 de 64 bits, un controlador gráfico con Vulkan 1.3 y un juego de Source instalado.

1. Descarga el zip más reciente desde la [página de versiones](https://github.com/riggs9162/SourceCubemap/releases) y descomprímelo donde quieras. Deja todos los archivos juntos.
2. Ejecuta `sourcecubemap-gui.exe`.
3. Elige tu `.bsp` y la carpeta de contenido de tu juego, por ejemplo `GarrysMod\garrysmod`.
4. Activa `Allow experimental rendering`, que es necesario para la mayoría de los mapas personalizados.
5. Pulsa `Generate cubemaps`, revisa las vistas previas de las caras y las advertencias, y luego valida el resultado con `Validate output`.
6. Copia el `.bsp` nuevo a la carpeta `maps` de tu juego. **No cambies el nombre de archivo generado**, porque los cubemaps están guardados justo bajo ese nombre.

## Línea de comandos

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| Opción | Qué hace |
| --- | --- |
| `--game FOLDER` | La carpeta de contenido de tu juego. Obligatoria. |
| `--profile gmod` o `hl2` | Qué juego marca las reglas. Por defecto, `gmod`. |
| `--experimental` | Sigue adelante con todo lo que no se pueda reproducir con exactitud y lo anota en el informe. |
| `--water planar` | Renderiza los reflejos del agua. Requiere `--experimental`. |
| `--hdr auto`, `ldr`, `hdr` o `both` | Qué iluminación generar. `auto` genera la que tenga el mapa. |
| `--quality fast`, `standard` o `high` | Elige entre velocidad y nitidez de las texturas. `high` renderiza al doble de resolución. |
| `--incremental` | Reutiliza los cubemaps que no han cambiado. |
| `--mount PATH` | Añade carpetas de contenido o paquetes adicionales. |
| `--previews FOLDER` | Guarda cada cara del cubemap como PNG. |
| `--json FILE` | Guarda un informe completo, con todas las advertencias. |
| `--output FILE` | Dónde escribir el mapa. Por defecto, `MAP_built.bsp`. |
| `--overwrite` | Sustituye un archivo de salida existente. |
| `--in-place` | Sustituye el mapa original, tras hacer una copia de seguridad. |

Ejecuta `sourcecubemap build --help` para ver la lista completa. `sourcecubemap inspect map.bsp` muestra lo que contiene un mapa sin compilar nada.

## Cuando una compilación se detiene

Por defecto, SourceCubemap se detiene si un mapa usa algo que no puede reproducir, y te dice qué es. Añade `--experimental` para continuar con lo más parecido que encuentre.

Con `--experimental`, los materiales o texturas de color que falten se renderizan en gris plano, y los props cuyo modelo falte se omiten. Cada sustitución aparece en el informe, así que revísalo antes de fiarte del resultado. Si ves que faltan muchos assets, monta con `--mount` el contenido que necesita el mapa.

Un mapa sin muestras de cubemap no tiene nada que generar, así que primero coloca entidades `env_cubemap` y vuelve a compilarlo. Los archivos corruptos siempre detienen la compilación.

## Qué renderiza

- Geometría del mundo, displacements, entidades de brush y props estáticos
- Lightmaps, tanto en LDR como en HDR
- Skyboxes 3D, niebla y cielos pintados de Garry's Mod
- Materiales habituales de Source, incluidos Phong y texturas de detalle
- Agua planar, sprites y glows
- Mapas comprimidos con la herramienta de reempaquetado de Valve

Solo se admiten mapas BSP de escritorio en versión 20. No se renderizan el gameplay, Lua, las partículas, las cuerdas ni los modelos animados. Consulta la [referencia técnica](../REFERENCE.md) (solo en inglés) para ver la cobertura exacta.

## Compilar desde el código fuente

Instala Visual Studio con la carga de trabajo «Desarrollo para el escritorio con C++» y luego ejecuta esto desde PowerShell:

```powershell
tools/build.ps1
```

El programa se genera en `build/release`. Usa `tools/build.ps1 -SkipConfigure` en las compilaciones siguientes. Las dependencias están fijadas a versiones concretas y no se incluye ningún asset de juegos.

## Documentación

- [Referencia técnica](../REFERENCE.md) (solo en inglés): cobertura de renderizado, cachés e informes
- [Arquitectura](../ARCHITECTURE.md) (solo en inglés): cómo encajan las piezas del proceso y cuáles son sus límites
- [Validación](../VALIDATION.md) (solo en inglés): resultados de pruebas y mediciones

Las versiones se numeran `A.B.cc`: `A` para cambios incompatibles, `B` para cambios compatibles con versiones anteriores y `cc` para correcciones.

## Licencia

MIT. Consulta [THIRD_PARTY.md](../../THIRD_PARTY.md) para ver las dependencias y las referencias.

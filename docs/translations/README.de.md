# SourceCubemap

*Dies ist eine Übersetzung. Die aktuellste Fassung ist immer die [englische README](../../README.md).*

[English](../../README.md) | Deutsch | [Español](README.es.md) | [Français](README.fr.md) | [Português (Brasil)](README.pt-BR.md) | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

Erzeuge Cubemaps für Source-Engine-Maps in Sekunden, ohne das Spiel zu starten.

SourceCubemap liest eine kompilierte `.bsp` und deine installierten Spielinhalte, rendert jede Cubemap auf deiner Grafikkarte und schreibt eine neue `.bsp` mit eingebauten Cubemaps. Deine Original-Map bleibt unangetastet.

| Map | Cubemaps | Zeit |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |
| Half-Life 2, 76 Maps | 6167 | 77 s insgesamt |

Gemessen mit einem Ryzen 9 7900 und einer RTX 3050.

> **Experimentell:** Die Reflexionen kommen dem Original sehr nahe, sind aber nicht pixelgenau identisch mit dem Ergebnis des Spielbefehls `buildcubemaps`. Teste deine Map im Spiel, bevor du sie veröffentlichst.

## Schnellstart

Du brauchst 64-Bit-Windows 10 oder 11, einen Grafiktreiber mit Vulkan 1.3 und ein installiertes Source-Spiel.

1. Lade die neueste ZIP-Datei von der [Releases-Seite](https://github.com/riggs9162/SourceCubemap/releases) herunter und entpacke sie an einen beliebigen Ort. Lass alle Dateien beieinander.
2. Starte `sourcecubemap-gui.exe`.
3. Wähle deine `.bsp` aus oder ziehe sie ins Fenster. Der Spielordner wird automatisch eingetragen, wenn die Map darin liegt. Sonst wähle ihn selbst aus, zum Beispiel `GarrysMod\garrysmod`.
4. Klicke auf `Generate cubemaps`.
5. Sieh dir die Vorschau und das Ergebnis daneben an. Unter `Options...` findest du Beleuchtung, Qualität, Wasser und zusätzliche Content-Ordner.
6. Kopiere die neue `.bsp` in den `maps`-Ordner deines Spiels. **Ändere den erzeugten Dateinamen nicht**, denn die Cubemaps sind unter genau diesem Namen gespeichert.

## Kommandozeile

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| Option | Was sie macht |
| --- | --- |
| `--game FOLDER` | Der Content-Ordner deines Spiels. Pflichtangabe. |
| `--profile gmod` oder `hl2` | Das Spiel, dessen Regeln gelten. Standard ist `gmod`. |
| `--experimental` | Macht bei allem weiter, was sich nicht exakt nachbilden lässt, und führt es im Bericht auf. |
| `--water planar` | Rendert Wasserreflexionen. Benötigt `--experimental`. |
| `--hdr auto`, `ldr`, `hdr` oder `both` | Welche Beleuchtung erzeugt wird. `auto` baut das, was die Map enthält. |
| `--quality fast`, `standard` oder `high` | Wählt zwischen Tempo und Texturschärfe. `high` rendert in doppelter Auflösung. |
| `--incremental` | Verwendet unveränderte Cubemaps wieder. |
| `--mount PATH` | Bindet zusätzliche Content-Ordner oder Archive ein. |
| `--previews FOLDER` | Speichert jede Cubemap-Seite als PNG. |
| `--json FILE` | Speichert einen vollständigen Bericht mit allen Warnungen. |
| `--output FILE` | Wohin die Map geschrieben wird. Standard ist `MAP_built.bsp`. |
| `--overwrite` | Ersetzt eine vorhandene Ausgabedatei. |
| `--in-place` | Ersetzt die Original-Map, nachdem eine Sicherung angelegt wurde. |
| `--no-backup` | Überspringt diese Sicherung, für Tools, die eine eigene Kopie der Map behalten. |

Führe `sourcecubemap build --help` aus, um die vollständige Liste zu sehen. `sourcecubemap inspect map.bsp` zeigt, was eine Map enthält, ohne etwas zu bauen.

## Mit CompilePal++ verwenden

Versionen von [CompilePal++](https://github.com/riggs9162/CompilePal) nach 029.4 können SourceCubemap als Kompilierschritt ausführen, sodass das Spiel beim Kompilieren nie starten muss. Die [Anleitung](https://github.com/riggs9162/CompilePal/blob/toolspp/standalone-support/docs/sourcecubemap.md) erklärt die Einrichtung.

## Wenn ein Build abbricht

Standardmäßig bricht SourceCubemap ab, wenn eine Map etwas verwendet, das es nicht nachbilden kann, und sagt dir, was es war. Mit `--experimental` macht es stattdessen mit der nächstbesten Entsprechung weiter. Das Windows-Programm macht das standardmäßig, und unter `Options...` lässt es sich abschalten.

Mit `--experimental` wird ein fehlendes Material oder eine fehlende Farbtextur einfarbig grau gerendert, und ein Prop mit fehlendem Modell wird weggelassen. Jeder Ersatz steht im Bericht, also sieh ihn dir an, bevor du dem Ergebnis vertraust. Wenn viele Assets fehlen, binde den Content, den die Map braucht, mit `--mount` ein.

Bei einer Map ohne Cubemap-Samples gibt es nichts zu erzeugen. Platziere also zuerst `env_cubemap`-Entities und kompiliere die Map neu. Beschädigte Dateien brechen den Build immer ab.

## Was gerendert wird

- Weltgeometrie, Displacements, Brush-Entities und Static Props
- Lightmaps, sowohl in LDR als auch in HDR
- 3D-Skyboxen, Nebel und die gemalten Himmel von Garry's Mod
- Gängige Source-Materialien, auch mit Phong und Detailtexturen
- Planares Wasser, Sprites und Glows
- Maps, die mit Valves Repacking-Tool komprimiert wurden

Unterstützt werden nur Desktop-Maps mit BSP-Version 20. Gameplay, Lua, Partikel, Seile und animierte Modelle werden nicht gerendert. Den genauen Umfang findest du in der [technischen Referenz](../REFERENCE.md) (nur auf Englisch).

## Aus dem Quellcode bauen

Installiere Visual Studio mit der Workload „Desktopentwicklung mit C++“ und führe dann in PowerShell Folgendes aus:

```powershell
tools/build.ps1
```

Das Programm landet in `build/release`. Nimm für spätere Builds `tools/build.ps1 -SkipConfigure`. Die Versionen der Abhängigkeiten sind fest vorgegeben, und es sind keine Spiel-Assets enthalten.

## Dokumentation

- [Technische Referenz](../REFERENCE.md) (nur auf Englisch): Rendering-Umfang, Caches und Berichte
- [Architektur](../ARCHITECTURE.md) (nur auf Englisch): wie die Pipeline zusammenspielt und wo ihre Grenzen liegen
- [Validierung](../VALIDATION.md) (nur auf Englisch): Testergebnisse und Messwerte

Releases sind nach dem Muster `A.B.cc` nummeriert: `A` für inkompatible Änderungen, `B` für abwärtskompatible Änderungen und `cc` für Fehlerbehebungen.

## Lizenz

MIT. Abhängigkeiten und Referenzen findest du in [THIRD_PARTY.md](../../THIRD_PARTY.md).

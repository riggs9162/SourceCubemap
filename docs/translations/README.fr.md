# SourceCubemap

*Ceci est une traduction. La version la plus à jour est toujours le [README en anglais](../../README.md).*

[English](../../README.md) | [Deutsch](README.de.md) | [Español](README.es.md) | Français | [Português (Brasil)](README.pt-BR.md) | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

Générez les cubemaps de vos maps Source Engine en quelques secondes, sans lancer le jeu.

SourceCubemap lit un `.bsp` compilé et le contenu de vos jeux installés, effectue le rendu de chaque cubemap sur votre carte graphique et écrit un nouveau `.bsp` qui intègre les cubemaps. Votre map d'origine reste intacte.

| Map | Cubemaps | Temps |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |
| Half-Life 2, 76 maps | 6167 | 77 s au total |

Mesuré avec un Ryzen 9 7900 et une RTX 3050.

> **Expérimental :** les reflets sont très proches du rendu réel, mais ils ne sont pas identiques au pixel près à ceux de la commande `buildcubemaps` du jeu. Testez votre map en jeu avant de la publier.

## Démarrage rapide

Il vous faut Windows 10 ou 11 en 64 bits, un pilote graphique compatible Vulkan 1.3 et un jeu Source installé.

1. Téléchargez le dernier zip depuis la [page des versions](https://github.com/riggs9162/SourceCubemap/releases) et décompressez-le où vous voulez. Gardez tous les fichiers ensemble.
2. Lancez `sourcecubemap-gui.exe`.
3. Choisissez votre `.bsp` ou déposez-le sur la fenêtre. Le dossier du jeu se remplit tout seul si la map s'y trouve. Sinon, choisissez-le vous-même, par exemple `GarrysMod\garrysmod`.
4. Cliquez sur `Generate cubemaps`.
5. Vérifiez l'aperçu et le résultat affiché à côté. `Options...` regroupe l'éclairage, la qualité, l'eau et les dossiers de contenu supplémentaires.
6. Copiez le nouveau `.bsp` dans le dossier `maps` de votre jeu. **Ne modifiez pas le nom de fichier généré**, car les cubemaps sont enregistrées précisément sous ce nom.

## Ligne de commande

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| Option | Ce qu'elle fait |
| --- | --- |
| `--game FOLDER` | Le dossier de contenu de votre jeu. Obligatoire. |
| `--profile gmod` ou `hl2` | Le jeu dont il faut suivre les règles. `gmod` par défaut. |
| `--experimental` | Continue malgré tout ce qui ne peut pas être reproduit à l'identique, et le signale dans le rapport. |
| `--water planar` | Effectue le rendu des reflets de l'eau. Nécessite `--experimental`. |
| `--hdr auto`, `ldr`, `hdr` ou `both` | L'éclairage à générer. `auto` génère ce que la map contient. |
| `--quality fast`, `standard` ou `high` | Choisit entre vitesse et netteté des textures. `high` effectue le rendu en résolution doublée. |
| `--incremental` | Réutilise les cubemaps qui n'ont pas changé. |
| `--mount PATH` | Ajoute des dossiers de contenu ou des archives supplémentaires. |
| `--previews FOLDER` | Enregistre chaque face de cubemap au format PNG. |
| `--json FILE` | Enregistre un rapport complet, avec tous les avertissements. |
| `--output FILE` | Où écrire la map. `MAP_built.bsp` par défaut. |
| `--overwrite` | Remplace un fichier de sortie existant. |
| `--in-place` | Remplace la map d'origine, après en avoir fait une sauvegarde. |

Lancez `sourcecubemap build --help` pour afficher la liste complète. `sourcecubemap inspect map.bsp` montre le contenu d'une map sans rien générer.

## Quand une génération s'arrête

Par défaut, SourceCubemap s'arrête si une map utilise quelque chose qu'il ne sait pas reproduire, et vous indique de quoi il s'agit. Ajoutez `--experimental` pour continuer plutôt avec l'équivalent le plus proche. Le programme Windows le fait par défaut, et `Options...` permet de le désactiver.

Avec `--experimental`, un matériau ou une texture de couleur manquants apparaissent en gris uni, et un prop dont le modèle manque est ignoré. Chaque remplacement figure dans le rapport : vérifiez-le avant de vous fier au résultat. S'il manque beaucoup d'assets, montez le contenu dont la map a besoin avec `--mount`.

Une map sans échantillons de cubemap n'a rien à générer : placez d'abord des entités `env_cubemap`, puis recompilez-la. Les fichiers corrompus arrêtent toujours la génération.

## Ce que le rendu inclut

- Géométrie du monde, displacements, entités de brush et props statiques
- Lightmaps, en LDR comme en HDR
- Skyboxes 3D, brouillard et ciels peints de Garry's Mod
- Matériaux Source courants, y compris le Phong et les textures de détail
- Eau planaire, sprites et glows
- Maps compressées avec l'outil de repacking de Valve

Seules les maps BSP version 20 pour ordinateur sont prises en charge. Le gameplay, Lua, les particules, les cordes et les modèles animés ne sont pas rendus. Consultez la [référence technique](../REFERENCE.md) (en anglais uniquement) pour le détail exact.

## Compiler depuis les sources

Installez Visual Studio avec la charge de travail « Développement Desktop en C++ », puis lancez ceci depuis PowerShell :

```powershell
tools/build.ps1
```

Le programme est écrit dans `build/release`. Utilisez `tools/build.ps1 -SkipConfigure` pour les compilations suivantes. Les dépendances sont figées sur des versions précises, et aucun asset de jeu n'est inclus.

## Documentation

- [Référence technique](../REFERENCE.md) (en anglais uniquement) : couverture du rendu, caches et rapports
- [Architecture](../ARCHITECTURE.md) (en anglais uniquement) : comment le pipeline s'articule, et ses limites
- [Validation](../VALIDATION.md) (en anglais uniquement) : résultats de tests et mesures

Les versions sont numérotées `A.B.cc` : `A` pour les changements incompatibles, `B` pour les changements rétrocompatibles et `cc` pour les correctifs.

## Licence

MIT. Consultez [THIRD_PARTY.md](../../THIRD_PARTY.md) pour les dépendances et les références.

# SourceCubemap

*Esta é uma tradução. A versão mais atualizada é sempre o [README em inglês](../../README.md).*

[English](../../README.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Français](README.fr.md) | Português (Brasil) | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

Gere cubemaps para mapas da Source Engine em segundos, sem abrir o jogo.

O SourceCubemap lê um `.bsp` compilado e o conteúdo dos seus jogos instalados, renderiza cada cubemap na sua placa de vídeo e grava um novo `.bsp` com os cubemaps embutidos. Seu mapa original continua intacto.

| Mapa | Cubemaps | Tempo |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |
| Half-Life 2, 76 mapas | 6167 | 77 s no total |

Medido com um Ryzen 9 7900 e uma RTX 3050.

> **Experimental:** os reflexos ficam bem próximos do resultado real, mas não são idênticos pixel a pixel aos do comando `buildcubemaps` do jogo. Teste seu mapa dentro do jogo antes de publicar.

## Início rápido

Você precisa do Windows 10 ou 11 de 64 bits, de um driver de vídeo com Vulkan 1.3 e de um jogo da Source instalado.

1. Baixe o zip mais recente na [página de releases](https://github.com/riggs9162/SourceCubemap/releases) e extraia onde quiser. Mantenha os arquivos juntos.
2. Execute `sourcecubemap-gui.exe`.
3. Escolha seu `.bsp` ou arraste-o para a janela. A pasta do jogo é preenchida sozinha quando o mapa está dentro dela. Caso contrário, escolha-a você mesmo, como `GarrysMod\garrysmod`.
4. Clique em `Generate cubemaps`.
5. Confira a prévia e o resultado ao lado. Em `Options...` ficam iluminação, qualidade, água e pastas de conteúdo extras.
6. Copie o novo `.bsp` para a pasta `maps` do seu jogo. **Não altere o nome de arquivo gerado**, porque os cubemaps ficam armazenados exatamente com esse nome.

## Linha de comando

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| Opção | O que faz |
| --- | --- |
| `--game FOLDER` | A pasta de conteúdo do seu jogo. Obrigatória. |
| `--profile gmod` ou `hl2` | De qual jogo seguir as regras. O padrão é `gmod`. |
| `--experimental` | Segue em frente com tudo o que não puder ser reproduzido com exatidão e lista no relatório. |
| `--water planar` | Renderiza os reflexos da água. Requer `--experimental`. |
| `--hdr auto`, `ldr`, `hdr` ou `both` | Qual iluminação gerar. `auto` gera o que o mapa tiver. |
| `--quality fast`, `standard` ou `high` | Escolhe entre velocidade e nitidez das texturas. `high` renderiza com o dobro da resolução. |
| `--incremental` | Reaproveita cubemaps que não mudaram. |
| `--mount PATH` | Adiciona pastas de conteúdo ou pacotes extras. |
| `--previews FOLDER` | Salva cada face do cubemap como PNG. |
| `--json FILE` | Salva um relatório completo, com todos os avisos. |
| `--output FILE` | Onde gravar o mapa. O padrão é `MAP_built.bsp`. |
| `--overwrite` | Substitui um arquivo de saída existente. |
| `--in-place` | Substitui o mapa original, depois de fazer um backup. |
| `--no-backup` | Pula esse backup, para ferramentas que mantêm a própria cópia do mapa. |

Execute `sourcecubemap build --help` para ver a lista completa. `sourcecubemap inspect map.bsp` mostra o que um mapa contém sem gerar nada.

## Quando o build é interrompido

Por padrão, o SourceCubemap interrompe o build se um mapa usar algo que ele não consegue reproduzir e informa o que foi. Adicione `--experimental` para continuar com a alternativa mais próxima. O programa para Windows já faz isso por padrão, e `Options...` permite desativar.

Com `--experimental`, materiais ou texturas de cor ausentes são renderizados em cinza liso, e props sem modelo ficam de fora. Cada substituição aparece no relatório, então confira antes de confiar no resultado. Se faltarem muitos assets, monte com `--mount` o conteúdo de que o mapa precisa.

Um mapa sem amostras de cubemap não tem nada a ser gerado, então coloque entidades `env_cubemap` e recompile o mapa primeiro. Arquivos corrompidos sempre interrompem o build.

## O que é renderizado

- Geometria do mundo, displacements, entidades de brush e props estáticos
- Lightmaps, tanto em LDR quanto em HDR
- Skyboxes 3D, neblina e os céus pintados do Garry's Mod
- Materiais comuns da Source, incluindo Phong e texturas de detalhe
- Água planar, sprites e glows
- Mapas compactados com a ferramenta de repack da Valve

Só são compatíveis mapas BSP de desktop na versão 20. Gameplay, Lua, partículas, cordas e modelos animados não são renderizados. Veja a [referência técnica](../REFERENCE.md) (somente em inglês) para saber a cobertura exata.

## Compilar a partir do código-fonte

Instale o Visual Studio com a carga de trabalho "Desenvolvimento para desktop com C++" e depois execute isto no PowerShell:

```powershell
tools/build.ps1
```

O programa é gravado em `build/release`. Use `tools/build.ps1 -SkipConfigure` nas próximas compilações. As dependências têm versões fixas, e nenhum asset de jogo está incluído.

## Documentação

- [Referência técnica](../REFERENCE.md) (somente em inglês): cobertura de renderização, caches e relatórios
- [Arquitetura](../ARCHITECTURE.md) (somente em inglês): como as partes do pipeline se encaixam e quais são seus limites
- [Validação](../VALIDATION.md) (somente em inglês): resultados de testes e medições

As versões são numeradas como `A.B.cc`: `A` para mudanças incompatíveis, `B` para mudanças compatíveis com versões anteriores e `cc` para correções.

## Licença

MIT. Veja [THIRD_PARTY.md](../../THIRD_PARTY.md) para dependências e referências.

# Hot Wheels Turbo Racing — recompilação estática (PS1 → C)

Recompilação estática de **Hot Wheels Turbo Racing (NTSC-U, SLUS-00964)** feita com ferramenta própria (MIPS → C).
O repositório contém **apenas a ferramenta e o runtime**. Nenhum dado, executável ou código gerado do jogo é distribuído:
tudo é gerado na sua máquina a partir do **seu** disco.

## O que funciona
Boot completo, filmes EA (.WVE), menus, corrida em 3D, áudio (SPU, XA e CD-DA), controle (teclado/gamepad) e memory card (`memcard1.mcd`).

## Como jogar (pacote de 1 clique)
Veja `kit/LEIA-ME.txt` (Windows) ou `kit/LEIA-ME-LINUX.txt`. Resumo: dê dois cliques em `Jogar.bat`, escolha o seu `.cue`.
Para ouvir a música, mantenha as faixas `Track 02…14 .bin` na mesma pasta do `.cue`.

## Estrutura
- `recomp.py` / `build_hwtr.py` — recompilador (gera `game.c`, `ov0.c`, `overlays.c`)
- `runtime.c`, `hw.c`, `gpu.c`, `gte.c`, `spu.c`, `mdec.c`, `xa.c`, `cdda.c`, `video.c` — runtime/hardware
- `kit/` — launcher de 1 clique (`jogar.py`), leitor de ISO, scripts de montagem do pacote

## Build manual
`python build_hwtr.py PASTA_COM_SLUS_009.64_E_CCCPSX.EXE SAIDA` e compile `SAIDA/*.c` + runtime com SDL2 (`-DUSE_SDL`).
Execute: `hwtr SLUS_009.64 --bin "disco (Track 01).bin" --janela --cd-turbo 10`.

Não é afiliado à Electronic Arts, Mattel ou Sony. Use apenas com uma cópia que você possui.

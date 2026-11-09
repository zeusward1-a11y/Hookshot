# Hookshot v2.0.5 com DLSS, cor certa e sem plantas brilhando

## Português

Este ramo é o Hookshot oficial no **b234b43** (main de 08/11/2026, v2.0.5 mais alguns commits) com 4 PRs que ainda não entraram e 1 correção minha. Todos os autores estão nos commits.

| Commit | Autor | O que faz |
| --- | --- | --- |
| [PR #759](https://github.com/NotRexed/Hookshot/pull/759) | pelicanux | Acaba com os caules e galhos brilhando em verde, ciano e vermelho ([#875](https://github.com/NotRexed/Hookshot/issues/875), [#874](https://github.com/NotRexed/Hookshot/issues/874), [#757](https://github.com/NotRexed/Hookshot/issues/757), [#667](https://github.com/NotRexed/Hookshot/issues/667)) |
| [PR #849](https://github.com/NotRexed/Hookshot/pull/849) | BlueHeisenberg | Linha "Output resolution" (2048x1152, 2560x1440, 3840x2160). Sem ela a imagem sai em 2048x1152 e é esticada: é daí que vem a maior parte da nitidez |
| [PR #850](https://github.com/NotRexed/Hookshot/pull/850) | BlueHeisenberg | NVIDIA DLSS no lugar do upscale do jogo (RTX) |
| Correção minha sobre a #850 | | O Streamline devolve `eWarnOutOfVRAM` (39) quando o DLSS **desenhou** o quadro mas o orçamento de VRAM do Windows acabou (`sl.common/commonInterface.cpp:552-560`). A #850 tratava isso como falha e desligava o DLSS até reiniciar; o menu passava a dizer "no NVIDIA DLSS found". Agora é só um aviso no log. Teste: `tests/test_dlss_result.py` |
| [PR #816](https://github.com/NotRexed/Hookshot/pull/816) | madebyjordan | A luz ambiente do céu se perdia em toda placa e a exposição clareava a cena (imagem lavada, [#36](https://github.com/NotRexed/Hookshot/issues/36)) |

### Testado

- **PC:** RTX 5070 12 GB, i5-14600KF, Windows 11, monitor 1440p.
- **DLSS:** `1920x1080 -> 2560x1440`, com "DLSS draws the upscale" no log.
- **Plantas:** em Kokiri estão com a cor certa.
- **Estabilidade:** nenhum device lost.
- **FPS:** em Kokiri fica perto de 40, porque o limite é a CPU, não a placa.

### Como usar

1. Compile este ramo do jeito de sempre.
2. Baixe o [Streamline SDK 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16) (`streamline-sdk-v2.14.16.zip`).
3. Crie uma pasta `streamline` ao lado do `hookshot.exe` e copie pra ela, de `bin\x64` do SDK: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` e `nvngx_dlss.dll`. As DLLs da NVIDIA não podem ir no repositório, e o jogo só carrega as que têm assinatura da NVIDIA.
4. No menu do jogo (Insert), aba Graphics, configure:
   - **Output resolution:** a do seu monitor
   - **DLSS:** On, e reinicie o jogo
   - **DLSS mode:** Quality, ou DLAA se a placa aguentar
   - **DLSS preset:** K ou M
5. **Visual mais escuro e menos enevoado (opcional):** isso já existe no Hookshot oficial, só vem desligado. Copie o [`docs/hookshot_env.txt`](docs/hookshot_env.txt) deste ramo pra pasta do jogo, ao lado do `hookshot.exe`. Ele tem estas linhas:

   ```
   RX_VK_IMAGE_BLOCK_MB=32
   RX_VK_OOT_VISUAL=1
   RX_VK_OOT_FOG=0.5
   RX_VK_OOT_EXPOSURE=0.85
   ```

   - `RX_VK_IMAGE_BLOCK_MB=32` vem da [PR #887](https://github.com/NotRexed/Hookshot/pull/887) e deixa menos VRAM parada.
   - `RX_VK_OOT_FOG` é a escala da névoa: 1 é a do jogo.
   - `RX_VK_OOT_EXPOSURE` é a escala da exposição: 1 é a do jogo, e 0.65 já fica escuro demais.
   - Mudar esses valores refaz o cache de shaders, então os primeiros minutos travam mais.
   - Pra desligar só a #816, acrescente `EDEN_F32_ADD_TRACK=0`.

São PRs ainda sem merge: podem mudar ou ser recusadas. Quando entrarem no oficial, volte pro `main`.

## English

This branch is official Hookshot at **b234b43** (main as of 2026-11-08, v2.0.5 plus a few commits), with 4 PRs that are not merged yet and 1 fix of mine. All authors are kept in the commits.

| What | Author |
| --- | --- |
| [#759](https://github.com/NotRexed/Hookshot/pull/759): glowing foliage stems fixed | pelicanux |
| [#849](https://github.com/NotRexed/Hookshot/pull/849): Output resolution row, which gives most of the sharpness | BlueHeisenberg |
| [#850](https://github.com/NotRexed/Hookshot/pull/850): NVIDIA DLSS | BlueHeisenberg |
| Fix on top of #850: Streamline's `eWarnOutOfVRAM` (39) means the frame **was** drawn and the VRAM budget is used up (`sl.common/commonInterface.cpp:552-560`). #850 treated it as a failure and turned DLSS off until restart; now it is only a log note. Test: `tests/test_dlss_result.py` | |
| [#816](https://github.com/NotRexed/Hookshot/pull/816): restores the sky's ambient light, so the scene is no longer washed out ([#36](https://github.com/NotRexed/Hookshot/issues/36)) | madebyjordan |

**Tested** on an RTX 5070 (12 GB) with an i5-14600KF, Windows 11, 1440p:
- DLSS upscales 1920x1080 to 2560x1440.
- Foliage has the right colours.
- No device lost.
- About 40 fps in Kokiri, limited by the CPU.

**Setup:**
1. Build this branch.
2. Download [Streamline SDK 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16).
3. Copy these four files from its `bin\x64` into a `streamline` folder next to `hookshot.exe`: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`.
4. In the Insert menu, open Graphics and set:
   - **Output resolution:** your monitor's
   - **DLSS:** On, then restart the game
   - **DLSS mode:** Quality
5. Optional, for a darker and less foggy look (built into official Hookshot, off by default): copy [`docs/hookshot_env.txt`](docs/hookshot_env.txt) next to `hookshot.exe`.

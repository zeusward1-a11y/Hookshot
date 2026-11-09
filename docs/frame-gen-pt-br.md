# Hookshot com DLSS, frame generation pelo OptiScaler e ritmo estável

Ramo `dlss-framegen-pacing` deste fork. Base: o Hookshot oficial em `8b2e1d3` (NotRexed/Hookshot), mais PRs abertas de desempenho
e o trabalho de DLSS e frame generation feito aqui. Testado num PC com RTX 5070 (12 GB), i5-14600KF e Windows 11, monitor
2560x1440 a 164 Hz. Este guia foi escrito para uma pessoa ou uma IA seguir passo a passo.

Nada aqui inclui o jogo nem arquivos da NVIDIA: você precisa da sua própria cópia do jogo (exefs e romfs) e de baixar os
arquivos da NVIDIA do site deles (seção 2).

## O que este ramo tem

| O quê | Como liga | Padrão |
|---|---|---|
| DLSS Super Resolution (Streamline 2.14.16, DLSS 310.9.16), presets J, K, L, M | menu, ou `RX_DLSS_PRESET=M` | menu |
| Jitter de câmera para o DLSS (Halton 2,3 só nos desenhos da cena) | `RX_DLSS_JITTER=1` | desligado |
| Movimento dos atores: Link e bichos com vetor de movimento próprio (porte da PR #841) | `RX_DLSS_ACTORS=0` desliga | ligado |
| Movimento capturado por transform feedback (experimental; não alcança a grama, que é desenhada por mesh shader) | `RX_DLSS_CAPTURE=1` | desligado |
| Teto de fps no ritmo do jogo, base estável para o frame generation | `HOS_FPS_CAP=<20..60>` | desligado |
| Orçamento de texturas que conta o resto da VRAM (DLSS-G, ponte, espelhos) | `RX_VK_VRAM_TARGET=<%>` | 85% |
| Prioridade de memória (PR #1132, não chegou a agir nos testes) | `RX_VK_MEMORY_PRIORITY=1` | desligado |
| Menu do jogo na tecla Home (no lugar do Insert, que é do OptiScaler) | Home, F1, ou Select+Start no controle | |

O frame generation que usamos NÃO é o do próprio Hookshot: é o DLSS-G da NVIDIA aplicado pelo OptiScaler modificado (seção 3),
que pega a imagem do jogo por uma ponte Vulkan para DirectX 12 e recebe as matrizes da câmera do jogo.

## 1. Compilar este ramo

O Hookshot é compilado a partir da sua cópia do jogo pelo Hookshot Builder. O jeito que usamos aqui: as ferramentas do Builder
(a pasta `tools`, com `emsdk`, `cpm`, `python`, `wgpu-native`) recebem as pastas `gpu`, `host`, `nx2`, `third_party`,
`scripts` e `assets` deste ramo, e o instalador do próprio projeto monta a pasta do jogo:

```bat
set T=<pasta tools do Hookshot Builder>
set EMSDK=%T%\emsdk
set RX_CPM=%T%\cpm
set WGPU_NATIVE=%T%\wgpu-native
cd /d %T%
python\python.exe -I -u -X utf8 nx2\make_game.py --exefs <exefs do jogo> --romfs <romfs do jogo> --out <pasta do jogo> --work <pasta de build> --jobs 8
```

Não testamos esse comando do zero com este ramo (aqui ele roda com uma tradução do jogo já feita, `nx2/build.py --standalone`);
uma IA consegue ajustar a partir do `nx2/make_game.py` e do `readme.md`. Um build completo leva da ordem de 45 minutos.

## 2. Arquivos da NVIDIA (baixe você mesmo)

Do Streamline SDK 2.14.16 (https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16, arquivo
`streamline-sdk-v2.14.16.zip`, 276.016.170 bytes), da pasta `bin/x64` (a de produção, não a `development`), copie para
`<pasta do jogo>\streamline\` estes 9 arquivos:

```
sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll  sl.reflex.dll  sl.pcl.dll
nvngx_dlss.dll  nvngx_dlssg.dll  NvLowLatencyVk.dll
```

O `nvngx_dlss.dll` certo é o 310.9.16, SHA-256 começando por `d00b5b0ab29c`. Usar esses arquivos é aceitar a licença da NVIDIA
que vem no SDK.

## 3. OptiScaler modificado (o frame generation)

Código: https://github.com/zeusward1-a11y/OptiScaler-F5-DLSSNR-Multipass/tree/hookshot-dx12-camera (fork do
janblade/OptiScaler-F5-DLSSNR-Multipass, base 0.1.35.1). Os 4 commits do ramo, em ordem:
- o DLSS-G em Vulkan, feito antes para o recomp do Tears of the Kingdom;
- a ponte Vulkan para DX12 com o upscaler do jogo: o DLSS do jogo é dono do quadro do frame generation, e o OptiScaler não
  regula o ritmo pelos marcadores do outro runtime;
- um experimento que ficou pior no jogo (a câmera do DLSS SR passada ao DLSS-G);
- a versão em uso: o DLSS-G recebe as matrizes da câmera do jogo, não a posição nem os vetores reconstruídos.

O `OptiScaler.dll` compilado está na release
https://github.com/zeusward1-a11y/OptiScaler-F5-DLSSNR-Multipass/releases/tag/hookshot-dx12-camera (é o que usamos aqui, build de
09/11 do último commit do ramo).

Instalação: a pasta `OptiScaler` da versão 0.1.35.1 do janblade ao lado do `hookshot.exe`, o `OptiScaler.dll` deste ramo
renomeado para `winmm.dll` ao lado do `hookshot.exe`, e um `OptiScaler.ini` com estas linhas (o resto no automático):

```ini
[Upscalers]
Dx12Upscaler = dlss
VulkanUpscaler = dlss
[FrameGen]
Enabled = true
FGInput = upscaler
FGOutput = dlssg
[DLSSG]
InterpolationCount = 1
UseGamesReflexMarkers = false
[Framerate]
FramerateLimit = 0
LowLatencyInput = optiscaler
[DLSS]
Enabled = true
[Libraries]
; o _nvngx.dll do SEU driver: C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_<número>\_nvngx.dll
NvngxPath = <caminho do _nvngx.dll do driver>
NvngxDlssPath = <pasta do jogo>\streamline\nvngx_dlss.dll
[Menu]
ShortcutKey = 0xDB
```

`ShortcutKey = 0xDB` põe o menu do OptiScaler na tecla `[`, livrando o Home para o menu do jogo.

## 4. `hookshot_env.txt` (ao lado do `hookshot.exe`)

O que usamos, com o motivo de cada linha:

```
RX_DLSSG=0                 # o DLSS-G do próprio Hookshot desligado: o frame generation é o do OptiScaler
RX_DLSS_NO_OTA=1           # obrigatório com o OptiScaler: dois Streamlines no mesmo processo brigam pelos plugins OTA e o jogo fecha
RX_DLSS_JITTER=1           # jitter para o DLSS: imagem mais nítida (sem custo medido)
RX_DLSS_PRESET=M           # preset do DLSS
RX_DLSS_OBJMV=0            # o "movimento próprio por profundidade" antigo piorava o fantasma: desligado
RX_VK_IMMEDIATE=1          # apresentação imediata (a ponte do OptiScaler roda com SyncInterval 0)
RX_VK_MIRROR_MB=2048       # espelhos de memória: 1024 deixava a CPU esperando a GPU ~5-6 ms por quadro (issue #995: 1024 = 34-48 fps, 2048 = 55)
HOS_OUTPUT_RES=1           # saída 2560x1440: a saída 4K custou ~1,9 GB de VRAM a mais numa placa de 12 GB
HOS_PIN=Presentation=FFF,MainThread=FFF,ModuleSystemWorker=FFF   # só em Intel híbrido: as threads nos núcleos P (máscara do seu processador)
#HOS_FPS_CAP=50            # opcional: base fixa em 50 (100 na tela em 2x); medido: variação de 4,4 para 1,3 fps em janelas de 5 s
```

Nas opções do jogo (`hookshot_options.ini`): `dlss=1`, `dlss_mode=3` (Quality), `dlss_frame_generation=0`.

## 5. Perfil da NVIDIA (NVIDIA Profile Inspector)

Um perfil para o `hookshot.exe` (caminho completo) com:
- `SL_DLSS_OVERRIDE` (0x10E41E06) = 0
- `VRR_APP_OVERRIDE` (0x10A879CF) = 0
- `VRR_APP_OVERRIDE_REQUEST_STATE` (0x10A879AC) = 0

VSync forçado pelo driver deu esperas de 50 ms: deixe no padrão.

## 6. Como medir

- `RX_DLSS_DEBUG=1`: vetores de movimento no lugar da imagem (cinza = parado); `=3`: erro de reprojeção (mais escuro = melhor).
- PresentMon 2.6 (como administrador): `PresentMon --process_name hookshot.exe --output_file pm.csv --timed 15
  --terminate_after_timed --v1_metrics`. Nos testes daqui: nenhum quadro perdido, 76 fps na tela, mediana 13,1 ms, p99 19 ms.
- O `hookshot_log.txt` traz a VRAM a cada 600 quadros (`vk: VRAM: ... used of ... allowed ...; the rest ..., the textures'
  room ...`) e o tempo de cada parte do quadro (`a frame's replay time (ms): ... memory sync ...`).

## 7. Problemas conhecidos

- O menu do jogo (Home) pode não abrir com a ponte do OptiScaler: o menu lê a tecla pelas mensagens da janela do jogo. Use
  as variáveis de ambiente acima para DLSS, preset e saída.
- A grama, as samambaias e as árvores do campo são desenhadas por mesh shaders e chegam ao frame generation só com o movimento
  da câmera: a grama alta balançando ainda "gelatina" um pouco.
- Em placas de 12 GB com DLSS-G, o castelo e a cidade enchem a VRAM; a saída em 1440p e os espelhos em 2048 são o que mais
  ajudou. Quando a VRAM passa de ~90% do que o driver permite, o jogo descarta e refaz texturas (travadas) e o frame
  generation piora.

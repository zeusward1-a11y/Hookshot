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

## 7. O que entrou em 10/11 (atualização do ramo)

Quinze commits em cima do que o guia descrevia. Nenhum mexe na assinatura do cache de shaders (nada é retraduzido).

**Cor da cena (nosso).** Um ajuste de cor na imagem que o DLSS recebe, só na cena 3D (a interface não muda) e só com o DLSS
ligado. No `hookshot_env.txt`, em porcentagem, aceitando negativo; sem as linhas, a cor é a do jogo:

```
RX_DLSS_GRADE_SAT=10       # saturação subtrativa: a cor fica mais forte e um pouco mais escura, como em película
RX_DLSS_GRADE_DENSITY=10   # quanto a cor saturada escurece (sem a linha, vale o número da saturação)
RX_DLSS_GRADE_CONTRAST=3   # contraste, pivô 0,435
```

**Erro 39 do DLSS.** Se o seu log mostra `vk: dlss: slEvaluateFeature failed (result 39): off from now on`, o seu código
não tem o commit "DLSS: Streamline's eWarnOutOfVRAM is a drawn frame, not a failure" (já estava neste ramo). O 39 é um
aviso de VRAM cheia que vem depois de o quadro ser desenhado; o código antigo tratava como falha e desligava o DLSS até
reiniciar. A frase do log é montada em `gpu/vk/vk_dlss.cpp` (`"%s failed (result %d)"`), por isso uma busca pela frase
inteira não acha. Com a correção o log passa a dizer `drawn over the VRAM budget`. Isso NÃO baixa o uso de VRAM.

**VRAM e travadas (commits de terceiros, do Hookshot oficial ou de PRs dele):**

| O quê | Autor | Como liga |
|---|---|---|
| Pools de memória aposentados devolvidos à placa na NVIDIA (issue 1057). Aqui devolveu 544 a 592 MB por vez | NotRexed | `RX_VK_KEEP_RETIRED=0` |
| Correção da limpeza de texturas em lotes (lote que estoura o tempo; conta do orçamento) | SadTransGirl | sempre |
| Imagens de render target reaproveitadas de uma reserva, em vez de criar e destruir 300 a 700 de uma vez (PR #1416, aberta no oficial) | DUDUYG | ligado; `RX_VK_TARGET_POOL_MB=<MB>` (padrão 1024, 0 desliga) |
| Arquivos de pipeline abertos antes de jogar (tirava uma parada de ~140 ms no começo; PR #1417, aberta no oficial) | DUDUYG | sempre |
| Pool de render target da casa do guarda (crash no Castle Town) e pool que acompanha a tela larga | nathanael.ries, NotRexed | sempre |
| O jogo fecha quando a GPU é perdida, em vez de tela preta | Gabriel Dias | sempre |
| `RX_VK_FRAME_LOG=<arquivo>`: uma linha de CSV por quadro apresentado, para medir o ritmo sem PresentMon | gelibeanz | env |

Medido aqui com a reserva do #1416 (RTX 5070, campo, 2 minutos): 2386 imagens reaproveitadas contra 1851 criadas, até 307 MB
guardados, fps médio igual ao de antes. O ganho nas travadas não foi medido por nós, só pelo autor do PR. A reserva é
devolvida quando a VRAM passa de 90% do que o driver permite.

Linhas de VRAM que usamos no `hookshot_env.txt`, além das da seção 4:

```
RX_VK_KEEP_RETIRED=0       # devolve os pools aposentados (conferir no log: "retired given back (N MB)" com N maior que 0)
RX_VK_IMAGE_BLOCK_MB=32    # blocos de imagem de 32 MB em vez de 128: menos memória parada na placa
```

Se a placa enche mesmo assim (log `Exceeded VRAM budget`, texturas acima do orçamento): `RX_VK_ASTC_BC_ALL=1` guarda todas
as texturas ASTC como BC3 (um quarto do tamanho, com um pouco menos de definição em textura fina) e `RX_VK_VRAM_MB=<MB>` fixa
o teto das texturas. Aqui os dois ficaram desligados depois de um teste A/B, mas são o que religar primeiro.

**Não entrou, de propósito:** a correção das árvores empilhadas do campo (PR #1334 do oficial) e a do ruído da folhagem
(PR #1338). Com elas a parede de árvores que pisca some, mas sobra só o plantio ralo; aqui preferimos a mata densa.

**DLSS5 NR (opcional, não faz parte deste ramo).** O addon de ReShade "DLSS5 NR" funciona neste jogo instalado como jogo
Vulkan (o ReShade entra como camada Vulkan, pede administrador uma vez). Ele enxerga o DLSS do jogo e roda depois do upscale,
com o frame generation do OptiScaler funcionando. Dois avisos: o instalador dele DESLIGA o frame generation no
`OptiScaler.ini` (volte `[FrameGen] Enabled = true`, `FGInput = upscaler`, `FGOutput = dlssg`); e custa caro: aqui a base
foi de ~57 para ~39 fps e a VRAM subiu para 9,3 de 10 GB.

## 8. Problemas conhecidos

- O menu do jogo (Home) pode não abrir com a ponte do OptiScaler: o menu lê a tecla pelas mensagens da janela do jogo. Use
  as variáveis de ambiente acima para DLSS, preset e saída.
- A grama, as samambaias e as árvores do campo são desenhadas por mesh shaders e chegam ao frame generation só com o movimento
  da câmera: a grama alta balançando ainda "gelatina" um pouco.
- Em placas de 12 GB com DLSS-G, o castelo e a cidade enchem a VRAM; a saída em 1440p e os espelhos em 2048 são o que mais
  ajudou. Quando a VRAM passa de ~90% do que o driver permite, o jogo descarta e refaz texturas (travadas) e o frame
  generation piora.

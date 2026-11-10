# Hookshot with DLSS, frame generation through OptiScaler, and steady pacing

Branch `dlss-framegen-pacing` of this fork. Base: upstream Hookshot at `8b2e1d3` (NotRexed/Hookshot), plus open performance PRs
and the DLSS and frame generation work done here. Tested on one PC: RTX 5070 (12 GB), i5-14600KF, Windows 11, a 2560x1440
monitor at 164 Hz. This guide is written so that a person or an AI can follow it step by step. (Portuguese original:
`docs/frame-gen-pt-br.md`.)

Nothing here includes the game or NVIDIA's files: you need your own copy of the game (exefs and romfs), and you download
NVIDIA's files from their site (section 2).

## What this branch has

| What | How to turn it on | Default |
|---|---|---|
| DLSS Super Resolution (Streamline 2.14.16, DLSS 310.9.16), presets J, K, L, M | menu, or `RX_DLSS_PRESET=M` | menu |
| Camera jitter for DLSS (Halton 2,3, only on the scene's draws) | `RX_DLSS_JITTER=1` | off |
| Actor motion: Link and creatures get their own motion vectors (a port of PR #841's idea) | `RX_DLSS_ACTORS=0` turns it off | on |
| Motion captured by transform feedback (experimental; does not reach the grass, which is drawn by mesh shaders) | `RX_DLSS_CAPTURE=1` | off |
| A frame rate cap at the game's own pace, a steady base for frame generation | `HOS_FPS_CAP=<20..60>` | off |
| A texture budget that counts the rest of the VRAM (DLSS-G, the bridge, mirrors) | `RX_VK_VRAM_TARGET=<%>` | 85% |
| Memory priorities (PR #1132; did not act in our tests) | `RX_VK_MEMORY_PRIORITY=1` | off |
| The game's menu on the Home key (instead of Insert, which is OptiScaler's) | Home, F1, or Select+Start on a controller | |

The frame generation used here is NOT Hookshot's own: it is NVIDIA's DLSS-G applied by a modified OptiScaler (section 3),
which takes the game's picture through a Vulkan to DirectX 12 bridge and receives the game's camera matrices.

## 1. Building this branch

Hookshot is built from your copy of the game by the Hookshot Builder. The way it is done here: the Builder's tools (its
`tools` folder, with `emsdk`, `cpm`, `python`, `wgpu-native`) receive this branch's `gpu`, `host`, `nx2`, `third_party`,
`scripts` and `assets` folders, and the project's own installer makes the game folder:

```bat
set T=<the Hookshot Builder's tools folder>
set EMSDK=%T%\emsdk
set RX_CPM=%T%\cpm
set WGPU_NATIVE=%T%\wgpu-native
cd /d %T%
python\python.exe -I -u -X utf8 nx2\make_game.py --exefs <the game's exefs> --romfs <the game's romfs> --out <game folder> --work <build folder> --jobs 8
```

We have not run this command from scratch with this branch (here it runs with the game's translation already made,
`nx2/build.py --standalone`); an AI can adjust it from `nx2/make_game.py` and `readme.md`. A full build takes on the order of
45 minutes.

## 2. NVIDIA's files (download them yourself)

From the Streamline SDK 2.14.16 (https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16, file
`streamline-sdk-v2.14.16.zip`, 276,016,170 bytes), from its `bin/x64` folder (the production one, not `development`), copy
these 9 files to `<game folder>\streamline\`:

```
sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll  sl.reflex.dll  sl.pcl.dll
nvngx_dlss.dll  nvngx_dlssg.dll  NvLowLatencyVk.dll
```

The right `nvngx_dlss.dll` is 310.9.16, its SHA-256 starting with `d00b5b0ab29c`. Using these files means accepting the
NVIDIA licence that comes with the SDK.

## 3. The modified OptiScaler (the frame generation)

Code: https://github.com/zeusward1-a11y/OptiScaler-F5-DLSSNR-Multipass/tree/hookshot-dx12-camera (a fork of
janblade/OptiScaler-F5-DLSSNR-Multipass, base 0.1.35.1). The branch's 4 commits, in order:
- DLSS-G in Vulkan, made earlier for the Tears of the Kingdom recomp;
- the Vulkan to DX12 bridge with the game's upscaler: the game's DLSS owns the frame generation's frame, and OptiScaler does
  not pace by the other runtime's markers;
- an experiment that was worse in play (the DLSS SR camera passed to DLSS-G);
- the version in use: DLSS-G receives the game's camera matrices, not its position or reconstructed vectors.

The compiled `OptiScaler.dll` is in the release
https://github.com/zeusward1-a11y/OptiScaler-F5-DLSSNR-Multipass/releases/tag/hookshot-dx12-camera (it is the one used here,
built on 9 November from the branch's last commit).

Installation: janblade's 0.1.35.1 `OptiScaler` folder beside `hookshot.exe`, this branch's `OptiScaler.dll` renamed to
`winmm.dll` beside `hookshot.exe`, and an `OptiScaler.ini` with these lines (the rest left on automatic):

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
; YOUR driver's _nvngx.dll: C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_<number>\_nvngx.dll
NvngxPath = <path of the driver's _nvngx.dll>
NvngxDlssPath = <game folder>\streamline\nvngx_dlss.dll
[Menu]
ShortcutKey = 0xDB
```

`ShortcutKey = 0xDB` puts OptiScaler's menu on the `[` key, leaving Home for the game's menu.

## 4. `hookshot_env.txt` (beside `hookshot.exe`)

What we use, with the reason for each line:

```
RX_DLSSG=0                 # Hookshot's own DLSS-G off: the frame generation is OptiScaler's
RX_DLSS_NO_OTA=1           # required with OptiScaler: two Streamlines in one process fight over the OTA plugins and the game closes
RX_DLSS_JITTER=1           # jitter for DLSS: a sharper picture (no measured cost)
RX_DLSS_PRESET=M           # the DLSS preset
RX_DLSS_OBJMV=0            # the older "own motion by depth" made ghosting worse: off
RX_VK_IMMEDIATE=1          # immediate presentation (OptiScaler's bridge runs with SyncInterval 0)
RX_VK_MIRROR_MB=2048       # memory mirrors: 1024 left the CPU waiting for the GPU ~5-6 ms a frame (issue #995: 1024 = 34-48 fps, 2048 = 55)
HOS_OUTPUT_RES=1           # 2560x1440 output: the 4K output cost ~1.9 GB more VRAM on a 12 GB card
HOS_PIN=Presentation=FFF,MainThread=FFF,ModuleSystemWorker=FFF   # hybrid Intel only: the threads on the P cores (your processor's mask)
#HOS_FPS_CAP=50            # optional: a fixed base of 50 (100 on screen at 2x); measured: variation from 4.4 to 1.3 fps over 5 s windows
```

In the game's options (`hookshot_options.ini`): `dlss=1`, `dlss_mode=3` (Quality), `dlss_frame_generation=0`.

## 5. NVIDIA profile (NVIDIA Profile Inspector)

A profile for `hookshot.exe` (full path) with:
- `SL_DLSS_OVERRIDE` (0x10E41E06) = 0
- `VRR_APP_OVERRIDE` (0x10A879CF) = 0
- `VRR_APP_OVERRIDE_REQUEST_STATE` (0x10A879AC) = 0

VSync forced by the driver gave 50 ms waits: leave it at its default.

## 6. How to measure

- `RX_DLSS_DEBUG=1`: motion vectors in place of the picture (grey = still); `=3`: reprojection error (darker = better).
- PresentMon 2.6 (as administrator): `PresentMon --process_name hookshot.exe --output_file pm.csv --timed 15
  --terminate_after_timed --v1_metrics`. In the tests here: no dropped frame, 76 fps on screen, median 13.1 ms, p99 19 ms.
- `hookshot_log.txt` has the VRAM every 600 frames (`vk: VRAM: ... used of ... allowed ...; the rest ..., the textures'
  room ...`) and the time of each part of the frame (`a frame's replay time (ms): ... memory sync ...`).

## 7. What was added on 10 November (branch update)

Fifteen commits on top of what the guide described. None touches the shader cache's signature (nothing is translated again).

**Scene colour (ours).** A colour adjustment on the picture DLSS receives, on the 3D scene only (the interface does not
change) and only with DLSS on. In `hookshot_env.txt`, in percent, negatives allowed; without the lines the colour is the
game's:

```
RX_DLSS_GRADE_SAT=10       # subtractive saturation: stronger and slightly darker colour, as on film
RX_DLSS_GRADE_DENSITY=10   # how much a saturated colour darkens (without the line, the saturation's number is used)
RX_DLSS_GRADE_CONTRAST=3   # contrast, pivot 0.435
```

**DLSS error 39.** If your log shows `vk: dlss: slEvaluateFeature failed (result 39): off from now on`, your code does not
have the commit "DLSS: Streamline's eWarnOutOfVRAM is a drawn frame, not a failure" (it was already in this branch). 39 is a
full-VRAM warning that comes after the frame has been drawn; the old code took it for a failure and turned DLSS off until a
restart. The log line is assembled in `gpu/vk/vk_dlss.cpp` (`"%s failed (result %d)"`), which is why a search for the whole
sentence finds nothing. With the fix the log says `drawn over the VRAM budget` instead. This does NOT lower VRAM use.

**VRAM and stutter (other people's commits, from upstream Hookshot or its PRs):**

| What | Author | How it is turned on |
|---|---|---|
| Retired memory pools given back to the card on NVIDIA (issue 1057). Here it gave back 544 to 592 MB at a time | NotRexed | `RX_VK_KEEP_RETIRED=0` |
| A fix to the batched texture clean-up (a batch that times out; the budget test) | SadTransGirl | always |
| Render target images reused from a pool instead of 300 to 700 made and destroyed at once (PR #1416, merged upstream since) | DUDUYG | on; `RX_VK_TARGET_POOL_MB=<MB>` (default 1024, 0 turns it off) |
| Pipeline files opened before play (removed a stall of ~140 ms early on; PR #1417, merged upstream since) | DUDUYG | always |
| The guard house's render target pool (a crash in Castle Town) and a pool that follows a wide picture | nathanael.ries, NotRexed | always |
| The game closes when the GPU device is lost, instead of a black screen | Gabriel Dias | always |
| `RX_VK_FRAME_LOG=<file>`: one CSV line per presented frame, to measure pacing without PresentMon | gelibeanz | env |

Measured here with #1416's pool (RTX 5070, Hyrule Field, 2 minutes): 2386 images reused against 1851 made, up to 307 MB
held, the same average fps as before. The gain in stutter was not measured by us, only by the PR's author. The pool is given
back when VRAM passes 90% of what the driver allows.

VRAM lines we use in `hookshot_env.txt`, besides section 4's:

```
RX_VK_KEEP_RETIRED=0       # gives the retired pools back (check the log: "retired given back (N MB)" with N above 0)
RX_VK_IMAGE_BLOCK_MB=32    # image blocks of 32 MB instead of 128: less memory idle on the card
```

If the card still fills up (log `Exceeded VRAM budget`, textures over their budget): `RX_VK_ASTC_BC_ALL=1` keeps every ASTC
texture as BC3 (a quarter of the size, with slightly less definition in fine textures) and `RX_VK_VRAM_MB=<MB>` fixes the
textures' ceiling. Both were left off here after an A/B test, but they are the first to turn back on.

**Not in this branch:** upstream's fix for the stacked trees of Hyrule Field (PR #1334) and for the foliage's noise pass
(PR #1338), both merged upstream since. With them the flickering wall of trees goes and the sparser, correct placement is
left. Take them from upstream if you want them; #1334 changes the translator, so the shader cache is translated again once.

**DLSS5 NR (optional, not part of this branch).** The ReShade add-on "DLSS5 NR" works in this game when installed as a
Vulkan game (ReShade comes in as a Vulkan layer and asks for administrator rights once). It sees the game's DLSS and runs
after the upscale, with OptiScaler's frame generation working. Two warnings: its installer turns frame generation OFF in
`OptiScaler.ini` (put back `[FrameGen] Enabled = true`, `FGInput = upscaler`, `FGOutput = dlssg`); and it is expensive: here
the base went from ~57 to ~39 fps and VRAM rose to 9.3 of 10 GB.

## 8. Known problems

- The game's menu (Home) may not open with OptiScaler's bridge: the menu reads the key from the game window's messages. Use
  the environment variables above for DLSS, the preset and the output.
- The grass, ferns and trees of the field are drawn by mesh shaders and reach frame generation with the camera's motion
  only: tall grass swaying still wobbles a little.
- On 12 GB cards with DLSS-G, the castle and the town fill the VRAM; the 1440p output and the mirrors at 2048 are what helped
  most. When VRAM passes ~90% of what the driver allows, the game discards and remakes textures (stutter) and frame
  generation gets worse.

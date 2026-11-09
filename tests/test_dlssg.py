"""NVIDIA DLSS Frame Generation (DLSS-G) through Streamline: its decisions (gpu/vk/dlssg_logic.h, built and run as
tests/dlssg_test.cpp) and how the renderer and the host are wired to them. No game, GPU or NVIDIA DLL.

The fixture checks the frame numbers (by the game's presents), the multiplier (RX_DLSSG, the menu row, the GPU's maximum),
Reflex's mode, when generation is off and why (menu, a frame DLSS did not draw, a swapchain change, a bad status), the options
sent (resources kept while off except for the player's Off), the queue plan Streamline's own vkCreateDevice would make, the
camera taken from the game's view-projection (orthonormal vectors, near/far/FOV, a D3D-style view-to-clip giving the same
depth) and the clip-to-previous-clip converted from OpenGL's z, and the log line every 600 presents.

The source checks: slInit loads DLSS-G, Reflex and PCL with frame-based tagging; extra queues are made, not refused; the window
surface goes through Streamline; one slSetConstants a frame shared with DLSS super resolution, numbered by presents; markers
around vkQueuePresentKHR; the built-in interpolation off while DLSS-G runs; the host's acquire reaches the renderer through a DLL
export, not through vk_api.h (which the shader caches' stamp hashes).

    python -I -B tests/test_dlssg.py [--cxx clang++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def block(text, marker):
    """The braces' body of the first function whose text starts at marker"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_sources():
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    dlss = read("gpu/vk/vk_dlss.cpp")
    start = block(dlss, "bool dlss_start()")
    need(re.search(r"features\[\]\s*=\s*\{sl::kFeatureDLSS,\s*sl::kFeatureDLSS_G,\s*sl::kFeatureReflex,\s*sl::kFeaturePCL\}", start),
         "slInit does not load DLSS, DLSS-G, Reflex and PCL")
    need("pref.numFeaturesToLoad = st.fg_asked ? 4 : 1;" in start, "DLSS-G's features are not loaded only when asked for")
    need("eUseFrameBasedResourceTagging" in start, "no frame-based resource tagging (slSetTagForFrame needs it)")
    need("DLSS wants extra queues" not in dlss, "extra queues are still refused")
    need("st.extra_graphics += req.vkNumGraphicsQueuesRequired" in start and
         "st.extra_compute += req.vkNumComputeQueuesRequired" in start and
         "st.extra_of += req.vkNumOpticalFlowQueuesRequired" in start, "the features' queue requirements are not summed")
    need(re.search(r"#define DLSSG_HOOKED\(X\) X\(vkCreateWin32SurfaceKHR\) X\(vkDestroySurfaceKHR\)", dlss),
         "the window surface does not go through Streamline")
    attach = block(dlss, "void dlss_attach(")
    need("info.graphicsQueueIndex = st.plan.graphics_index" in attach and "info.computeQueueIndex = st.plan.compute_index" in attach
         and "info.useNativeOpticalFlowMode = true" in attach, "slSetVulkanInfo is not told where Streamline's queues are")
    evaluate = block(dlss, "bool dlss_evaluate(")
    need("fr.constants_frame.load() != frame" in evaluate and evaluate.count("st.set_constants(") == 1,
         "the frame's constants are not set once a frame in dlss_evaluate")
    need("st.set_tag(*ft, viewport, tags, 2" in evaluate, "depth and motion are not tagged for DLSS-G")
    need("get_new_frame_token(frame, &frame_index)" not in evaluate, "the frame token is not the presents' number")
    begin = block(dlss, "void dlssg_present_begin(")
    need("dlssg::decide(p)" in begin and "p.menu_open = menu_is_open();" in begin and "p.tagged = fr.tagged_frame.load() == frame;" in begin,
         "the present does not decide generation from the menu and the frame's tags")
    need("sl::DLSSGFlags::eRetainResourcesWhenOff" in begin, "generation is turned off without eRetainResourcesWhenOff")
    need(begin.index("eRenderSubmitEnd") < begin.index("ePresentStart"), "render-submit-end does not come before present-start")
    need("sl::ResourceTag{nullptr, sl::kBufferTypeDepth" in begin, "frames DLSS did not draw get no null tags")
    end = block(dlss, "void dlssg_present_end(")
    need("ePresentEnd" in end and "numFramesActuallyPresented" in end and "fr.stats.line(" in end,
         "the present's end has no present-end marker, frame count or log")
    fstart = block(dlss, "void dlssg_frame_start()")
    need(fstart.index("st.reflex_sleep(") < fstart.index("eSimulationStart"), "Reflex's sleep is not before the simulation start")
    need("frame_of(fr.game_presents" in fstart, "the frame start is not numbered by the game's presents")

    replay = read("gpu/vk/vk_replay.cpp")
    pi = block(replay, "void present_image(")
    at = pi.index("res = vkQueuePresentKHR(dev.queue, &pi);")
    need(pi.rfind("dlssg_present_begin(", 0, at) != -1 and pi.find("dlssg_present_end(game_frame)", at) != -1,
         "the DLSS-G present calls are not around vkQueuePresentKHR")
    need("dlss_defer_swapchain()" in pi, "a resize does not turn DLSS-G off first")
    need("present_image(src, sw, sh, crop, direct_view, (uint32_t)presents);" in replay, "do_present does not pass the frame's number")
    need("dlss_note_present((uint64_t)presents);" in block(replay, "void do_present("), "do_present does not count the replay's presents")
    need("dlssg_game_present();" in block(replay, "void present(const nvn_tex* t"), "the game's presents are not counted")

    gate = block(read("gpu/vk/vk_framegen.cpp"), "bool framegen_on(")
    need("!dlssg_engaged()" in gate, "the built-in interpolation runs while DLSS-G does")

    draw = read("gpu/vk/vk_draw.cpp")
    need("dlss_evaluate(cmd, color, depth, motion, output, reset, dlss_frame_id(), view);" in draw,
         "DLSS's evaluation is not numbered by presents, or has no camera")
    need("VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, cmd))" in draw,
         "the motion image cannot be copied by Streamline (no TRANSFER_SRC)")

    main = read("gpu/vk/vk_main.cpp")
    need("dlss_queues(dev.physical, dev.queue_family, families" in main and "ci.pQueueCreateInfos = queues.infos.data();" in main,
         "the device is not made with Streamline's queues")
    need("VK_EXT_DEBUG_UTILS_EXTENSION_NAME" in main, "no VK_EXT_debug_utils with DLSS-G")
    need("dlss_swapchain_created();" in block(main, "bool create_swapchain("), "Reflex's mode is not set again after a swapchain")
    need(re.search(r"void mkwvk_frame_start\(void\) \{\s*mkwvk::dlssg_frame_start\(\);", main), "no mkwvk_frame_start export")

    backend = read("host/rx_backend.c")
    need('GetProcAddress(m, "mkwvk_frame_start")' in backend and "void rx_frame_start(void)" in backend,
         "the host does not find the renderer's frame start")
    acquire = block(read("host/nvn_native.c"), "NV(nvnWindowAcquireTexture)")
    need(acquire.index("hos_vi_native_acquire(") < acquire.index("rx_frame_start();"), "the frame start is not after the host's pacing")

    # (the shader caches' stamp hashes vk_api.h, nx2/build.py: no new renderer table entry, the caches stay valid)
    api = read("gpu/vk/vk_api.h")
    need("frame_start" not in api and "#define MKWVK_API_VERSION 4" in api, "vk_api.h changed: the shader caches would be thrown away")

    menu = read("gpu/vk/vk_menu.cpp")
    need('"DLSS Frame Generation", &opts.dlssg, 0, 4, 1, tx_dlssg' in menu, "no DLSS Frame Generation row")
    need('dlssg::parse_multiplier(getenv("RX_DLSSG")' in menu, "RX_DLSSG does not set the row")
    need("dlssg::menu_step(*r.value, press[4])" in menu, "the row does not step Off, 2x, 3x, 4x")
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    failures = check_sources()
    for f in failures:
        print("FAIL:", f)
    with tempfile.TemporaryDirectory(prefix="dlssg-") as tmp:
        exe = Path(tmp) / ("dlssg_test.exe" if os.name == "nt" else "dlssg_test")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/dlssg_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)])
    if failures or r.returncode:
        raise SystemExit("%d wiring failures, fixture exit %d" % (len(failures), r.returncode))
    print("PASS: DLSS-G logic and wiring")


if __name__ == "__main__":
    main()

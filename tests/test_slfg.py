"""NVIDIA DLSS Frame Generation through Streamline (gpu/vk/vk_slfg.cpp), beside NGX's DLSS Super Resolution: its decisions
(gpu/vk/dlssg_logic.h, built and run as tests/dlssg_test.cpp) and how the renderer and the host are wired to it. No game, GPU or
NVIDIA DLL.

The source checks: Streamline loads frame generation's three features and no super resolution, and nothing when it is not asked
for; the row is read from hookshot_options.ini before the instance, RX_DLSSG over it; Reflex's limit counts every present unless
told; NGX's evaluation hands its depth, motion and camera on; the present's begin and end sit around vkQueuePresentKHR with the
game frame's number; the game's own Reflex and frame interpolation stand aside; the host's acquire reaches the renderer through
a DLL export.

    python -I -B tests/test_slfg.py [--cxx clang++]
"""
import argparse
import re
import shutil
import subprocess
import sys
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


def code(text):
    """text with its // comments left out: a call commented away is no call"""
    return re.sub(r"//[^\n]*", "", text)


def check_sources():
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    slfg = read("gpu/vk/vk_slfg.cpp")
    need("namespace slfg {" in slfg and "namespace slfg {" in read("gpu/vk/vk_slfg.h"), "vk_slfg is not in a namespace of its own")
    start = code(block(slfg, "bool dlss_start()"))
    need(re.search(r"features\[\]\s*=\s*\{sl::kFeatureDLSS_G,\s*sl::kFeatureReflex,\s*sl::kFeaturePCL\}", start),
         "Streamline does not load DLSS-G, Reflex and PCL alone (super resolution is NGX's)")
    need("pref.numFeaturesToLoad = 3;" in start, "not three features")
    need("eUseFrameBasedResourceTagging" in start, "no frame-based resource tagging (slSetTagForFrame needs it)")
    asked = start.index("st.fg_asked = sopts.dlssg >= 2;")
    need(asked < start.index("return off(", asked) < start.index("LoadLibrary"), "Streamline is loaded when frame generation is not asked for")
    need("sl::kFeatureDLSS," not in start and "get_feature_requirements(sl::kFeatureDLSS," not in start,
         "the start still asks for Streamline's super resolution")
    # the row: saved in hookshot_options.ini, read before the instance (the menu is made after it), RX_DLSSG over it
    saved = code(block(slfg, "int saved_dlssg()"))
    need('"dlss_frame_generation="' in saved and "hookshot_options.ini" in saved and "game_module_path" in saved,
         "the row is not read from the game folder's hookshot_options.ini")
    need('dlssg::multiplier_of(getenv("RX_DLSSG") && *getenv("RX_DLSSG") ? atoi(getenv("RX_DLSSG")) : saved_dlssg())' in slfg,
         "the multiplier is not RX_DLSSG's, else the saved row's")
    menu = read("gpu/vk/vk_menu.cpp")
    need('{TAB_GRAPHICS, "DLSS Frame Generation", &opts.dlssg, 0, 4, 1, tx_dlssg,' in menu, "no DLSS Frame Generation row")
    row = menu[menu.index('"DLSS Frame Generation", &opts.dlssg'):]
    need("nullptr, true, true}," in row[:row.index("},") + 2], "the row is not marked as read at the start (and preset exempt)")
    need("int dlssg = 0;" in read("gpu/vk/vk_menu.h"), "no opts.dlssg")
    # while it runs, the rows it stands in for have nothing to choose, and say whose work it is
    fixed = code(block(menu, "bool row_fixed(const int* value)"))
    need("if (dlssg_takes_over() && (value == &opts.framegen || value == &opts.pace || value == &opts.reflex)) return true;" in fixed,
         "Frame generation, Frame pacing and NVIDIA Reflex can still be changed while DLSS Frame Generation runs")
    need("bool dlssg_takes_over() { return slfg::dlssg_engaged(); }" in menu, "the menu does not ask whether DLSS-G runs")
    need('return dlssg_takes_over() ? "Off (DLSS Frame Generation)" : t[v];' in block(menu, "const char* tx_framegen(int v)"),
         "the Frame generation row does not say DLSS Frame Generation makes the frames")
    need("dlssg_takes_over()" in block(menu, "const char* tx_pace(int v)") and '"Frame pacing", &opts.pace, 0, 1, 1, tx_pace,' in menu,
         "the Frame pacing row does not say whose pacing it is")
    need('if (dlssg_takes_over()) return "On (DLSS Frame Generation\'s)";' in block(menu, "const char* tx_reflex(int v)"),
         "the NVIDIA Reflex row reads Off while Streamline's Reflex runs")
    # Reflex's limit: every present unless told
    need('st.limit_basis = dlssg::parse_limit(limit && *limit ? limit : "present");' in slfg,
         "Reflex's frame limit does not count every present by default")
    # NGX's evaluation hands its inputs on
    dlss = read("gpu/vk/vk_dlss.cpp")
    up = code(block(dlss, "bool dlss_upscale("))
    ev = up.index("NGX_VULKAN_EVALUATE_DLSS_EXT(")
    give = up.index("slfg::dlssg_frame_inputs(cmd, gd, gm, rw, rh, reset, slfg::dlss_frame_id(), gv)")
    need(ev < up.index("s.failed = true;", ev) < give, "the inputs are given before the evaluation is known to have worked")
    need("dlss_math::unjitter(g_current, cam.jitter);" in up and "gv.view_projection = g_current;" in up,
         "the camera given is not the view-projection without its jitter")
    need("if (!reset && dlss_math::reprojection(g_current, previous, g_to_previous)) gv.clip_to_previous = g_to_previous;" in up,
         "no clip-to-previous-clip, or one over a reset")
    for image in ("s.depth", "s.motion"):
        made = up[up.index("!make_image(cmd, %s," % image):]
        need("VK_IMAGE_USAGE_TRANSFER_SRC_BIT" in made[:made.index("||")], "%s cannot be copied by Streamline (no TRANSFER_SRC)" % image)
    inputs = code(block(slfg, "bool dlssg_frame_inputs("))
    need(inputs.index("if (!dlssg_engaged()) return false;") < inputs.index("st.set_constants("), "constants are set with generation off")
    need("fr.constants_frame.load() != frame" in inputs, "slSetConstants may run twice in a frame")
    need(inputs.count("sl::ResourceLifecycle::eOnlyValidNow") == 2, "the depth and motion are not copied when tagged")
    # the present
    replay = read("gpu/vk/vk_replay.cpp")
    pres = code(block(replay, "void present_image("))
    begin, end = pres.index("slfg::dlssg_present_begin(game_frame,"), pres.index("slfg::dlssg_present_end(game_frame);")
    presents = [m.start() for m in re.finditer(r"vkQueuePresentKHR\(", pres)]
    need(presents and all(begin < p < end for p in presents), "a present is not between DLSS-G's begin and end")
    need(pres.index("slfg::dlssg_window_check();") < pres.index("dev.swap_resize"), "the window is not checked first")
    need("const uint64_t want = sl_wait ? 0 : dev.swap_resize.exchange(0, std::memory_order_acquire);" in pres,
         "a resize does not wait for generation to go off")
    need("present_image(src, sw, sh, crop, direct_view, (uint32_t)presents);" in code(replay), "the game frame's number is not passed")
    need("slfg::dlss_note_present((uint64_t)presents);" in code(replay), "the replay's presents are not counted for the frame's number")
    need("slfg::dlssg_game_present();" in code(block(replay, "void present(const nvn_tex* t, const int crop[4])")),
         "the game's presents are not counted")
    need("uint32_t game_frame = 0);" in read("gpu/vk/vk_framegen.h"), "a made frame's present does not default to no game frame")
    # the swapchain
    main = read("gpu/vk/vk_main.cpp")
    sc = code(block(main, "bool create_swapchain(uint32_t width, uint32_t height)"))
    need(sc.index("slfg::dlss_swapchain_changing();") < sc.index("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"),
         "generation is not turned off before the swapchain changes")
    need(sc.index("if (slfg::dlssg_available())") < sc.index("VkSwapchainCreateInfoKHR ci{"), "DLSS-G does not choose the present mode")
    need("slfg::dlss_swapchain_created();" in sc, "the new swapchain is not told")
    dev = code(main)
    need(dev.index("if (slfg::dlss_start())") < dev.index("vkCreateInstance(&ci"), "Streamline is not started before the instance")
    need("ci.pQueueCreateInfos = sl_plan ? sl_queues.infos.data() : qs;" in dev, "Streamline's queues are not made")
    need(dev.index("vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);") < dev.index("slfg::dlss_attach("),
         "Streamline is not given the device after it is made")
    # the game's own Reflex and frame interpolation stand aside
    need("if (slfg::dlssg_requested()) return 0;" in code(block(read("gpu/vk/vk_reflex.cpp"), "int wanted_mode()")),
         "the game's own Reflex runs beside Streamline's")
    need("&& !slfg::dlssg_engaged();" in code(read("gpu/vk/vk_framegen.cpp")), "the built-in interpolation runs beside DLSS-G")
    # the host's frame start: a DLL export, looked up by name
    need('void mkwvk_frame_start(void) {\n    mkwvk::slfg::dlssg_frame_start();' in main, "no mkwvk_frame_start export")
    backend = code(block(read("host/rx_backend.c"), "void rx_frame_start(void)"))
    need('GetProcAddress(vk_module, "mkwvk_frame_start")' in backend, "the host does not look the export up")
    acquire = code(block(read("host/nvn_native.c"), "NV(nvnWindowAcquireTexture)"))
    need(acquire.index("hos_vi_native_acquire(") < acquire.index("rx_frame_start();") < acquire.index("hos_vcore_back();"),
         "the frame does not start after the pacing, off the game's core")
    # the build
    build = read("nx2/build.py")
    need('"third_party", "streamline", "include")' in build, "the build does not give the back end Streamline's headers")
    need('"-lversion", "-lwintrust", "-lcrypt32"' in build, "the back end is not linked with what vk_slfg.cpp calls")
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++"))
    a = p.parse_args()
    failures = check_sources()
    if a.cxx:
        with tempfile.TemporaryDirectory(prefix="slfg-") as tmp:
            exe = Path(tmp) / "dlssg_test.exe"
            subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu" / "vk"),
                            str(ROOT / "tests" / "dlssg_test.cpp"), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            print(r.stdout, end="")
            if r.returncode:
                failures.append("tests/dlssg_test.cpp failed")
    else:
        failures.append("no C++ compiler: dlssg_logic.h not run")
    for f in failures:
        print("FAIL:", f)
    print("test_slfg:", "FAILED" if failures else "ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

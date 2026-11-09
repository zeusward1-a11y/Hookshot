"""HOS_FPS_CAP (host/hos_fps.h, used by host/nn_vi.c): the frame rate held at most the cap on the display queue the presents
are paced by, a steady base for frame generation. The cap read (20..60, else none) and the queue's period (the game's interval
of 60 Hz vsyncs, or the cap's frame time when longer); then nn_vi.c's own hos_vi_native_presented and hos_vi_native_acquire,
compiled out of the source with a clock that sleeps move, pacing a game that renders a frame in 10 ms: 60 fps uncapped, the
cap's rate capped, one log line for a cap set (held, ignored, or of no effect with HOS_NO_PACING).

python -I -B tests/test_fps_cap.py [--cc gcc] [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_present_trace as trace  # noqa: E402  (its stubs of the game's runtime for nn_vi.c's presents)

CASES = r'''
#include <stdio.h>
#include "hos_fps.h"
static int failures;
#define CHECK(x) do { if (!(x)) printf("FAIL line %d: %s\n", __LINE__, #x), failures++; } while (0)
int main(void) {
    CHECK(hos_fps_cap_of(NULL) == 0 && hos_fps_cap_of("") == 0 && hos_fps_cap_of("50") == 50 && hos_fps_cap_of("45") == 45);
    CHECK(hos_fps_cap_of("19") == 0 && hos_fps_cap_of("61") == 0 && hos_fps_cap_of("-50") == 0 && hos_fps_cap_of("50x") == 0);
    CHECK(hos_fps_cap_of("20") == 20 && hos_fps_cap_of("60") == 60 && hos_fps_cap_of("120") == 0);
    /* no cap: the game's interval of 60 Hz vsyncs, as before */
    CHECK(hos_fps_period_ns(1, 0) == 16666666 && hos_fps_period_ns(2, 0) == 33333332 && hos_fps_period_ns(0, 0) == 16666666);
    /* a cap: its frame time when longer than the vsyncs */
    CHECK(hos_fps_period_ns(1, 50) == 20000000 && hos_fps_period_ns(1, 45) == 22222222 && hos_fps_period_ns(1, 60) == 16666666);
    /* the game asking for two vsyncs (30 fps) keeps them under a cap of 50 */
    CHECK(hos_fps_period_ns(2, 50) == 33333332);
    if (failures) return printf("%d failures\n", failures), 1;
    printf("hos_fps: ok\n");
    return 0;
}
'''

# (a game that renders a frame in 10 ms: acquire, render, present, 300 times; the mean present interval of the last 200)
PACING = r'''
int main(void) {
    clock_now = 1000000000LL;
    int64_t at[300];
    for (int i = 0; i < 300; ++i) {
        hos_vi_native_acquire(3);
        clock_now += 10000000LL;
        hos_vi_native_presented();
        at[i] = clock_now;
    }
    printf("MEAN %lld LOGS %d LAST %s\n", (long long)((at[299] - at[99]) / 200), cap_logs, cap_last);
    return 0;
}
'''


def pacing_source():
    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    prefix = trace.PREFIX
    sleep = "static void hos_sleep_ns(int64_t ns) { (void)ns; ++sleeps; }"
    log = "static void hos_log(const char *fmt, ...) { (void)fmt; }"
    if sleep not in prefix or log not in prefix:
        raise SystemExit("FAIL: test_present_trace.py's stubs changed: update this test")
    prefix = "#include <stdarg.h>\n" + prefix.replace(sleep, "static void hos_sleep_ns(int64_t ns) { clock_now += ns; ++sleeps; }")
    prefix = prefix.replace(log, r'''static int cap_logs;
static char cap_last[256] = "-";
static void hos_log(const char *fmt, ...) {
    char b[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (strstr(b, "HOS_FPS_CAP")) {
        ++cap_logs;
        for (char *c = b; *c; ++c) if (*c == ' ') *c = '_';
        snprintf(cap_last, sizeof cap_last, "%s", b);
    }
}''')
    helper = trace.block(vi, "static void present_trace(") if "static void present_trace(" in vi else ""
    return (prefix + (ROOT / "host" / "hos_fps.h").read_text(encoding="utf-8") + helper + "\n" +
            trace.block(vi, "void hos_vi_native_acquire(int active) {") + "\n" + trace.block(vi, "void hos_vi_native_presented(") +
            "\n" + PACING)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="fps-cap-") as tmp:
        src, exe = Path(tmp) / "fps_cap.c", Path(tmp) / "fps_cap.exe"
        src.write_text(CASES, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"), str(src), "-o", str(exe)], check=True)
        if subprocess.run([str(exe)]).returncode:
            failures.append("hos_fps's cases")
        cpp, sim = Path(tmp) / "pacing.cpp", Path(tmp) / "pacing.exe"
        cpp.write_text(pacing_source(), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(cpp), "-o", str(sim)], check=True)

        def run(env_extra):
            env = {k: v for k, v in os.environ.items() if k not in ("HOS_FPS_CAP", "HOS_NO_PACING", "HOS_VI_DOUBLE", "HOS_PRESENT_TRACE",
                                                                    "HOS_FRAME_SLEEP_MS", "HOS_GAME_INTERVAL")}
            env.update(env_extra)
            out = subprocess.run([str(sim)], env=env, capture_output=True, text=True, check=True).stdout.split()
            return int(out[1]) / 1e6, int(out[3]), out[5]

        cases = [  # (env, the mean present interval in ms, log lines, what the log says)
            ({}, 16.667, 0, "-"),
            ({"HOS_FPS_CAP": "50"}, 20.0, 1, "held"),
            ({"HOS_FPS_CAP": "45"}, 22.222, 1, "held"),
            ({"HOS_FPS_CAP": "60"}, 16.667, 1, "held"),
            ({"HOS_FPS_CAP": "120"}, 16.667, 1, "ignored"),
            ({"HOS_FPS_CAP": "abc"}, 16.667, 1, "ignored"),
            ({"HOS_FPS_CAP": "50", "HOS_VI_DOUBLE": "1"}, 20.0, 1, "held"),
            ({"HOS_FPS_CAP": "50", "HOS_NO_PACING": "1"}, 10.0, 1, "no_effect"),
        ]
        for env, ms, logs, says in cases:
            got_ms, got_logs, last = run(env)
            print(f"{env or 'unset'}: {got_ms:.3f} ms a frame, {got_logs} log line(s): {last}")
            if abs(got_ms - ms) > 0.01 or got_logs != logs or says not in last:
                failures.append(f"{env}: {got_ms:.3f} ms, {got_logs} logs ({last}); wanted {ms} ms, {logs}, '{says}'")
    if failures:
        for f in failures:
            print("FAIL:", f)
        raise SystemExit(f"{len(failures)} failures")
    print("PASS: HOS_FPS_CAP paces the display queue")


if __name__ == "__main__":
    main()

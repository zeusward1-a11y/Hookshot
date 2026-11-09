"""Releases never pass a stop they cannot see, and a capture with late pages always waits for them (local, on PR 958 and 936).

PR 958 took sync_order_mu off texture_released_of: a release now asks blocked_n alone. run_submit marks a queue stopped
under replay_mu, but blocked_n was set only by update_blocked after run_submit returned (submit_q). A release in that window
read 0, went to do_texture_released, took replay_mu as soon as run_submit let it go, and freed a texture the stopped rest of
the submission still uses. So: run_submit says the stop in blocked_n at once (sync mode), and the release asks again under
replay_mu and releases under that same lock, where no stop can come between the answer and the release.

PR 936 made the replay, not the game's thread, wait for pages a submission still on the GPU writes back (late_value). A
capture with no copy (past 256 MB, an area's load, or no buffer) skipped that wait, and its pages were read from guest memory
before the write-back landed: the exposure flash at a load. take_capture waits for late_value with or without a copy.
python -I -B tests/test_held_release_order.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, start=0):
    start = text.index(marker, start)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def code(text):
    """The text without // comments (a comment that says the right thing is not the code doing it)"""
    return re.sub(r"//[^\n]*", "", text)


def check_stop_said_at_once(replay):
    body = code(block(replay, "bool run_submit("))
    # (the stop's own block, if (stop) { ... }, and the line at its top level: not in its else (a submission that went on),
    # not under an inner if such as the first 12 stops' log)
    stopped = block(body, "if (stop) {")
    inner = stopped[stopped.index("{") + 1:]
    said = re.search(r"if \(sync_waits_on\(\) && !blocked_n\.load\(\)\) blocked_since = std::chrono::steady_clock::now\(\), "
                     r"blocked_n = 1;", inner)
    if not said:
        raise SystemExit("run_submit: a queue's stop is not said in blocked_n before run_submit lets replay_mu go")
    before = inner[:said.start()]
    if before.count("{") != before.count("}"):
        raise SystemExit("run_submit: blocked_n is set under an inner condition, not at every stop")
    if "q.waiting = true;" not in before:
        raise SystemExit("run_submit: blocked_n is set before the queue is marked stopped")
    if "return" in before:
        raise SystemExit("run_submit: a return comes between the stop and blocked_n")


def check_release(replay, marker, release):
    body = code(block(replay, marker))
    sync = body.index("if (!async_on())")
    held = body.index("held_mu", sync)
    asked = re.search(r"if \(!blocked_n\.load\(\)\) \{\s*std::lock_guard lk\(replay_mu\);\s*if \(!blocked_n\.load\(\)\) return "
                      + re.escape(release) + r"\(", body[sync:held])
    if not asked:
        raise SystemExit(f"{marker.strip('(')}: blocked_n is not asked again under replay_mu before the release")
    if f"do_{release}" in body[sync:held] or "do_texture_released" in body[sync:held]:
        raise SystemExit(f"{marker.strip('(')}: the release takes replay_mu again after the answer (a stop can come between)")
    head = body[sync:held]
    if "hold_releases() && sync_waits_on() && blocked_n.load()" in head:
        raise SystemExit(f"{marker.strip('(')}: the hold still depends on blocked_n read without replay_mu alone")


def check_under_lock_release(replay):
    body = code(block(replay, "void texture_released_under_replay_mu("))
    inside = body[body.index("{"):]   # (the name says replay_mu: only what it does counts)
    if "lock_guard" in inside or "replay_mu" in inside:
        raise SystemExit("texture_released_under_replay_mu takes a lock: its callers hold replay_mu already")
    if "texture_released(t->serial)" not in body or "texture_released(t);" not in body:
        raise SystemExit("texture_released_under_replay_mu does not do what do_texture_released did")
    wrapper = code(block(replay, "void do_texture_released(const nvn_tex* t) {"))
    if "std::lock_guard lk(replay_mu);" not in wrapper or "texture_released_under_replay_mu(t);" not in wrapper:
        raise SystemExit("do_texture_released no longer releases under replay_mu")


def check_capture_wait(memory):
    body = code(block(memory, "bool take_capture("))
    first = body.index("std::lock_guard lk(pools_mu);")
    head = body[:first]
    if not re.search(r"if \(c->late_value && !c->map\) vk_timeline_wait\(c->late_value\);", head):
        raise SystemExit("take_capture: a capture with late pages and no copy does not wait for their write-back")
    if not re.search(r"if \(c->late_value && c->map\) \{\s*vk_timeline_wait\(c->late_value\);", head):
        raise SystemExit("take_capture: the copied late pages are not copied again after their write-back")


def main():
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    memory = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    check_stop_said_at_once(replay)
    check_under_lock_release(replay)
    check_release(replay, "void texture_released_of(", "texture_released_under_replay_mu")
    check_release(replay, "void program_released_locked(", "program_released")
    check_capture_wait(memory)
    print("test_held_release_order: ok")


if __name__ == "__main__":
    main()

"""Compile the real delay and mix biquad filter of host/nn_audio.c with synthetic signals, without a device or the game.

python -I -B tests/test_audio_effects.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_audio.c")
    parser.add_argument("--run", default="", help="a runner for the built program (e.g. node, for an emcc build)")
    args = parser.parse_args()
    process_flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0

    def run(command):
        result = subprocess.run(command, creationflags=process_flags, capture_output=True,
                                text=True, encoding="utf-8", errors="replace")
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
    source = args.source.read_text(encoding="utf-8")
    definitions = []
    for name in ("FRAME", "RATE", "MAX_CH", "MAX_BUF"):
        matches = re.findall(r"^#define " + name + r"\s+\d+[^\n]*", source, re.M)
        assert len(matches) == 1, name
        definitions.extend(matches)
    mix = re.search(r"^typedef struct \{\n(?:.*\n)*?\} Mix;\n", source, re.M)
    assert mix, "the Mix type"
    start, end = source.index("/* ---- delay:"), source.index("static int fx_seq")
    actual = "\n".join(definitions) + "\n" + mix.group(0) + source[start:end]
    temporary = tempfile.TemporaryDirectory(prefix="audio-effects-")
    folder = Path(temporary.name).resolve()
    try:
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("audio-effects-")
        (folder / "audio_effects.inc").write_text(actual, encoding="utf-8")
        executable = folder / ("fixture.js" if args.run else "fixture.exe")
        run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(folder),
             str(ROOT / "tests/audio_effects_test.c"), "-o", str(executable)] + ([] if os.name == "nt" else ["-lm"]))
        run(([args.run] if args.run else []) + [str(executable)])
    finally:
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("audio-effects-")
        temporary.cleanup()


if __name__ == "__main__":
    main()

"""The core hos_reserved_cpus keeps for the replay (host/hos_thread.c pick_reserved_core): the last core of the fastest kind.

On a CPU of one kind every core has the same EfficiencyClass and the last core is kept, as before; on Intel's hybrid CPUs
the efficiency cores come last and have the lower class, so the last performance core is kept instead.

python -I -B tests/test_reserved_core.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'host/hos_thread.c')
    args = parser.parse_args()
    helper = block(args.source.read_text(encoding='utf-8'), 'static uint64_t pick_reserved_core(')
    fixture = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + helper + r'''
struct Core { uint64_t mask; unsigned char cls; };
uint64_t pick(const std::vector<Core>& cores) {
    std::vector<uint64_t> masks;
    std::vector<unsigned char> classes;
    for (const Core& c : cores) masks.push_back(c.mask), classes.push_back(c.cls);
    return pick_reserved_core(masks.data(), classes.data(), (int)cores.size());
}
// n cores of `per` logical processors each from logical processor `first`, all of class cls
void add(std::vector<Core>& v, int n, int per, int first, unsigned char cls) {
    for (int i = 0; i < n; i++) v.push_back({((1ull << per) - 1) << (first + i * per), cls});
}
int main() {
    std::vector<Core> v;
    add(v, 12, 2, 0, 0);   // Ryzen 9 3900X: 12 cores with SMT, one kind
    CHECK(pick(v) == 0xC00000);
    v.clear();
    add(v, 6, 2, 0, 0);    // Ryzen 5 7600X
    CHECK(pick(v) == 0xC00);
    v.clear();
    add(v, 8, 1, 0, 0);    // 8 cores without SMT
    CHECK(pick(v) == 0x80);
    v.clear();
    add(v, 8, 2, 0, 1);    // i7-12700K: 8 performance cores with HT (0-15), then 4 efficiency cores (16-19)
    add(v, 4, 1, 16, 0);
    CHECK(pick(v) == 0xC000);
    v.clear();
    add(v, 8, 2, 0, 1);    // i9-13900K: 8 performance cores with HT (0-15), then 16 efficiency cores (16-31)
    add(v, 16, 1, 16, 0);
    CHECK(pick(v) == 0xC000);
    v.clear();
    add(v, 4, 1, 0, 0);    // efficiency cores listed first, then performance cores: still the last fast one
    add(v, 2, 2, 4, 1);
    CHECK(pick(v) == 0xC0);
    CHECK(pick({}) == 0);
    CHECK(pick({{0, 1}, {0x3, 0}}) == 0x3);   // (an empty mask is no core)
    std::puts("PASS: the last core of the fastest kind, the last core on CPUs of one kind");
}
'''
    with tempfile.TemporaryDirectory(prefix='reserved-core-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()

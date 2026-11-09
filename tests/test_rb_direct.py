"""CPU check of write-back runs cut around the pages bound directly in guest memory.

python -I -B tests/test_rb_direct.py [--cxx clang++]
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
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
const int kPageShift = 12;
std::set<uint32_t> direct;
bool enabled = true;
bool rb_direct_on() { return enabled; }
bool rb_direct_page(uint32_t page) { return direct.count(page) != 0; }
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + block(text, 'void cut_rb_direct(') + r'''
using Runs = std::vector<VkBufferCopy>;
Runs cut(Runs runs, uint64_t pool_lo) { cut_rb_direct(runs, pool_lo); return runs; }
bool same(const Runs& a, const Runs& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].srcOffset != b[i].srcOffset || a[i].dstOffset != b[i].dstOffset || a[i].size != b[i].size) return false;
    return true;
}
int main() {
    const uint64_t lo = 0x100000;   // (page 0x100 at offset 0)
    CHECK(same(cut({{0, 0, 0x3000}}, lo), {{0, 0, 0x3000}}));              // nothing direct: one run, whole
    direct = {0x101};
    CHECK(same(cut({{0, 0, 0x3000}}, lo), {{0, 0, 0x1000}, {0x2000, 0x2000, 0x1000}}));   // a page in the middle
    CHECK(same(cut({{0x800, 0x800, 0x1000}}, lo), {{0x800, 0x800, 0x800}}));   // a run starting and ending inside pages
    CHECK(cut({{0x1000, 0x1000, 0x1000}}, lo).empty());                    // all of it direct
    direct = {0x100, 0x102};
    CHECK(same(cut({{0, 0, 0x4000}}, lo), {{0x1000, 0x1000, 0x1000}, {0x3000, 0x3000, 0x1000}}));
    CHECK(same(cut({{0, 0, 0x1000}, {0x1000, 0x1000, 0x1000}}, lo), {{0x1000, 0x1000, 0x1000}}));   // two runs
    direct = {0x101};
    CHECK(same(cut({{0x10, 0x10, 0x20}}, lo + 0x1000 - 0x1000), {{0x10, 0x10, 0x20}}));   // a small run clear of the page
    enabled = false;
    CHECK(same(cut({{0, 0, 0x3000}}, lo), {{0, 0, 0x3000}}));              // RX_VK_RB_DIRECT=0
    std::puts("rb_direct ok");
}
'''
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / 't.cpp', Path(d) / 't'
        src.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-o', str(exe), str(src)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()

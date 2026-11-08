"""CPU check of write-back runs cut around replay-owned report words.

python -I -B tests/test_report_words.py [--cxx clang++]
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
    helper = block(text, 'void cpu_words_merge(') + chr(10) + block(text, 'void cut_cpu_words(')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>
struct Pool { uint64_t lo; };
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
std::mutex cpu_words_mu;
std::vector<uint64_t> cpu_words, cpu_words_new;   // (sorted, and noted since: vk_memory.cpp)
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + helper + r'''
void check(uint64_t start, uint64_t end, std::initializer_list<uint64_t> owned) {
    const Pool pool{0x1000};
    cpu_words.clear();
    cpu_words_new.clear();
    std::set<uint64_t> owned_words;
    for (uint64_t offset : owned) cpu_words_new.push_back(pool.lo + offset), owned_words.insert(pool.lo + offset);
    std::vector<VkBufferCopy> runs{{start, start, end - start}};
    cut_cpu_words(pool, runs);
    std::set<uint64_t> copied;
    uint64_t previous_end = start;
    for (const auto& run : runs) {
        CHECK(run.srcOffset == run.dstOffset && run.size);
        CHECK(run.srcOffset >= previous_end && run.srcOffset + run.size <= end);
        for (uint64_t byte = run.srcOffset; byte < run.srcOffset + run.size; ++byte)
            CHECK(copied.insert(byte).second);
        previous_end = run.srcOffset + run.size;
    }
    for (uint64_t byte = 0; byte < 96; ++byte) {
        const bool expected = byte >= start && byte < end && !owned_words.count((pool.lo + byte) & ~7ull);
        CHECK(copied.count(byte) == expected);
    }
}
int main() {
    check(20, 24, {16});        // run starts inside an owned word
    check(20, 32, {16});        // keep the nonowned suffix
    check(12, 20, {16});        // partial end: keep the nonowned prefix
    check(16, 24, {16});        // entire word
    check(0, 48, {8, 16, 32});  // adjacent words, then a nonowned gap
    check(24, 32, {16});        // touching is not overlap
    check(0, 8, {16});          // nonoverlap
    check(0, 48, {});           // no ownership
    for (uint64_t start = 0; start < 80; start += 4)
        for (uint64_t end = start + 4; end <= 80; end += 4) {
            check(start, end, {16});
            check(start, end, {8, 16, 32, 40});
        }
    std::puts("PASS: partial report-word overlaps excluded; every nonowned byte preserved");
}
'''
    with tempfile.TemporaryDirectory(prefix='report-words-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()

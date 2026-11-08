"""Fault-inject actual layout creation and its callers, with no Vulkan driver or game.

python -I -B tests/test_layout_failures.py --cxx clang++ --headers /path/to/Vulkan-Headers/include
Optional --baseline-source compiles the old helper too, for success-argument comparison
and an original-failure negative; it is not needed for normal CI.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = None
HEADERS = None
BASELINE = None


def block(text, marker):
    """Extract one exact production brace block; these selected blocks have no braces in strings."""
    if text.count(marker) != 1:
        raise AssertionError(f"expected one production marker: {marker}")
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def generated_files(directory):
    source = (ROOT / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    shader = (ROOT / 'gpu/vk/vk_shader.cpp').read_text(encoding='utf-8')
    header = (ROOT / 'gpu/vk/vk_shader.h').read_text(encoding='utf-8')
    draw_header = (ROOT / 'gpu/vk/vk_draw.h').read_text(encoding='utf-8')
    types = '\n'.join(block(header, m) + ';' for m in ('struct Stage {', 'struct Program {'))
    types += '\n' + block(draw_header, 'struct PipeNote {') + ';'
    types += '\n' + block(source, 'struct Targets {') + ';'
    types += '\n' + block(source, 'struct PipeEntry {') + ';'
    (directory / 'layout_types.inc').write_text(types, encoding='utf-8')
    push = next(line for line in source.splitlines() if line.startswith('constexpr uint32_t kPushSize ='))
    helper = block(source, 'bool ensure_layout(Program& p) {')
    (directory / 'layout_helper.inc').write_text(push + '\n' + helper, encoding='utf-8')

    # These wrappers retain the production return statements, conditions, cleanup
    # and dependent calls. Sentinels observe whether control passed the guard;
    # they do not simulate the rendering/scheduling algorithms after that point.
    draw = block(source, '    PipeEntry* pe;\n    {')
    dispatch = block(source, '    Program& p = const_cast<Program&>(*cp);\n    if (!p.layout) {')
    warm = block(source, '    const auto warm_compute = [&](Program& p) {') + ';'
    seed = block(source, '            if (std::unique_ptr<Program> p = seed_program(i / 3, (int)(i % 3))) {')
    callback = block(source, '    const size_t programs_n = program_cache_load_some(wanted, [&](Program& p) {') + ');'
    pre = block(source, '    struct Pre {') + ';'
    worker = block(source, '    const auto worker = [pre, total] {') + ';'
    predict = block(source, '            program_cache_borrow(keys[i], [&](Program& p) {') + ');'
    borrow = block(shader, 'void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn) {')
    skip = next(line for line in source.splitlines() if line.startswith('#define SKIP_HERE()'))
    wrappers = skip + '\n' + borrow + '\n'
    wrappers += 'void call_draw(Program& p) { NvnState s{}; Targets at{}; VkPrimitiveTopology topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;\n' + draw + '\n(void)pe; ++after_guard; }\n'
    wrappers += 'void call_dispatch(Program* cp) {\n' + dispatch + '\n++after_guard; }\n'
    wrappers += 'void call_warm(Program& p) { bool warm_cs=true; auto& libs=counts.libs;\n' + warm + '\nwarm_compute(p); }\n'
    wrappers += 'void call_seed() { size_t i=0; bool warm=true; auto blank=std::make_unique<NvnState>(); auto& made=counts.made; auto& libs=counts.libs;\n' + seed + '\n}\n'
    wrappers += 'void call_precompile(std::vector<std::unique_ptr<PipeNote>> notes) { std::unordered_set<uint64_t> wanted; std::vector<Program*> computes;\n' + callback + '\n(void)programs_n;\n' + pre + '\nconst size_t total=notes.size()+computes.size(); Pre* pre=new Pre; pre->notes=std::move(notes); pre->computes=std::move(computes);\n' + worker + '\nworker(); delete pre; }\n'
    wrappers += 'void call_predict(const std::vector<const PipeNote*>& list) { std::vector<uint64_t> keys{7}; size_t i=0; auto& made=counts.made;\n' + predict + '\n}\n'
    (directory / 'layout_callers.inc').write_text(wrappers, encoding='utf-8')
    baseline = ''
    if BASELINE:
        baseline = block(BASELINE.read_text(encoding='utf-8'), 'void ensure_layout(Program& p) {')
        baseline = baseline.replace('void ensure_layout(', 'void ensure_layout_original(', 1)
    (directory / 'layout_baseline.inc').write_text(baseline, encoding='utf-8')


class LayoutFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise AssertionError('a C++20 compiler is required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='hookshot-layout-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name).resolve()
        generated_files(directory)
        cls.executable = directory / 'layout-check.exe'
        command = [compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-missing-field-initializers', '-DVK_NO_PROTOTYPES', '-DNDEBUG',
                   '-I' + str(directory), '-I' + str(HEADERS),
                   '-I' + str(ROOT / 'gpu/vk/compat'), '-I' + str(ROOT / 'gpu/compat'),
                   '-I' + str(ROOT / 'gpu/vk/eden'), '-I' + str(ROOT / 'gpu/vk'),
                   '-I' + str(ROOT / 'gpu/third_party/suyu'),
                   str(ROOT / 'tests/layout_failures_test.cpp'), '-o', str(cls.executable)]
        if os.name == 'nt':
            command += ['-D_CRT_SECURE_NO_WARNINGS']  # Match production's Windows CRT contract for getenv.
        if BASELINE:
            command += ['-DHAVE_ORIGINAL_HELPER']
        options = {'creationflags': subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == 'nt' else {}
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, **options)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name, *args):
        result = subprocess.run([str(self.executable), name, *args], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_transaction_and_retry(self):
        self.run_case('transaction')

    def test_success_arguments_and_fast_path(self):
        self.run_case('success')
        if BASELINE:
            result = subprocess.run([str(self.executable), 'original-negative'], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('original helper called a dependent API after failure', result.stderr)

    def test_draw_and_dispatch_return(self):
        self.run_case('draw-dispatch')

    def test_compute_warming_cleanup(self):
        self.run_case('warm')

    def test_graphics_seed_cleanup(self):
        self.run_case('seed')

    def test_cached_compute_and_graphics_notes(self):
        self.run_case('cached')

    def test_prediction_borrow_cleanup(self):
        self.run_case('predict')

    def test_bounded_result_logging(self):
        self.run_case('logging')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx')
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--baseline-source', type=Path)
    parser.add_argument('--original-negative', action='store_true')
    args, remaining = parser.parse_known_args()
    COMPILER, HEADERS, BASELINE = args.cxx, args.headers, args.baseline_source
    if args.original_negative:
        if not BASELINE:
            parser.error('--original-negative needs --baseline-source')
        LayoutFailureTests.setUpClass()
        try:
            result = subprocess.run([str(LayoutFailureTests.executable), 'original-negative'], capture_output=True, text=True, timeout=15)
            print(result.stdout + result.stderr, end='')
            if result.returncode != 1 or 'original helper called a dependent API after failure' not in result.stderr:
                raise SystemExit('original negative did not fail for the expected contract')
        finally:
            LayoutFailureTests.doClassCleanups()
    else:
        unittest.main(argv=[__file__, *remaining])

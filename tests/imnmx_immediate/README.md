# Wide native IMNMX immediates

The native lowerer already accepts `0x817` with signed 20-bit literals. This
fixture targets the missing full 32-bit range, not a missing opcode family.
Ordinary low-register signed 20-bit cases retain the existing one-word encoding;
wide literals use MOV32I and the existing register IMNMX. Literal-role correction
also covers high-alias signed 20-bit cases where old scratch classification was wrong.

The public [pinned Mesa NAK encoder](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/b1fe47e94409d81924c8663f58fac97e66ab4527/src/nouveau/compiler/nak/sm70_encode.rs#1729)
uses base `0x017` with ALU immediate form 4, producing `0x817`.
`encode_alu_imm` places the complete literal in bits 32–63. The signed selector is
bit 73; the min predicate and inversion occupy bits 87–90. These are authored
encoding fields, not recovered game instructions.

The narrow lowering retains existing mode checks: PT/inverted-PT selects min/max;
other selector predicates, unsupported width/modifier bits, and malformed source
forms remain refused. The execution predicate is independently preserved on both
materialization and IMNMX, including `@!PT` omission. No generic allocator policy
or register/cbuf/uniform path is changed.

`tests/test_imnmx_immediate.py --cxx clang++` compiles the production header in
lowering-only mode. The full CMake target additionally creates actual native
programs, runs the real REGMAP/Maxwell CFG/TranslateProgram/optimization pipeline,
checks typed min/max operands and their dataflow into shared-memory stores, then
emits SPIR-V. A predicated literal may pass through transparent SSA merges;
arithmetic transformations of its bits are not accepted by the fixture.

The 57 planned modules cover signed/unsigned min/max boundaries, source/destination
aliases, native R251 versus lowering scratch R251, neighboring and uniform state,
literal byte `0xfa`, high literal bits, and dynamic/disabled execution predicates.
Constants folded by the actual pipeline are checked numerically; otherwise the
typed IR operation, exact operands and observable store dependency are checked.
This is translation/module validation, not execution of SPIR-V on a GPU.

```sh
CC=clang CXX=clang++ cmake -S tests/imnmx_immediate -B build-imnmx \
  -DREPO_ROOT="$PWD" -DSPIRV_HEADERS_ROOT=/path/to/SPIRV-Headers
cmake --build build-imnmx --parallel 2
build-imnmx/imnmx_immediate build-imnmx/modules
for module in build-imnmx/modules/*.spv; do
  spirv-val --target-env vulkan1.3 "$module"
done
```

The original header refuses a wide authored literal with `native integer min/max
mode`. `--expect-original-refusal` checks that intended control. Run
`--existing-controls <folder>` against original and changed headers, then compare
the authored binary outputs to verify ordinary signed 20-bit, register, cbuf and
uniform forms remain identical. No dump, game, GPU, SDK instruction material or
shader cache is used. Supporting this range does not establish why an observed
game shader failed, or claim every `0x817` mode is supported.

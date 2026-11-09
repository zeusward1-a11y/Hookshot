// (hookshot) DLSS's captured motion (vk_draw.cpp dlss_capture_*): a vertex stage's SPIR-V made to write its clip position to a
// transform feedback buffer, so the game's own shader records where each vertex of a draw lands, every frame (the wind of the
// field's grass, rigid parts that move without bones). Added: Capability TransformFeedback, ExecutionMode Xfb on the vertex
// entry point, and on the Position output XfbBuffer 0, XfbStride 16 and Offset 0 (a member of the gl_PerVertex block, or a
// variable of its own). Nothing else changes: the shader computes and writes as before. Header only, no Vulkan:
// tests/test_spirv_xfb.py patches glslang's vertex shaders and checks the words; tests/dlss_capture_gpu.cpp runs them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spirv_xfb {

constexpr uint32_t kMagic = 0x07230203;
constexpr uint32_t OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeStruct = 30, OpTypePointer = 32,
                   OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
                   OpGroupMemberDecorate = 75, OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633;
constexpr uint32_t DecorationBuiltIn = 11, DecorationOffset = 35, DecorationXfbBuffer = 36, DecorationXfbStride = 37;
constexpr uint32_t BuiltInPosition = 0, StorageOutput = 3, ModelVertex = 0, CapabilityTransformFeedback = 53, ModeXfb = 11;

inline uint32_t op(uint32_t w) { return w & 0xffff; }
inline uint32_t len(uint32_t w) { return w >> 16; }
inline bool annotation(uint32_t o) {
    return o == OpDecorate || o == OpMemberDecorate || o == OpDecorationGroup || o == OpGroupDecorate || o == OpGroupMemberDecorate ||
           o == OpDecorateId || o == OpDecorateString || o == OpMemberDecorateString;
}

/// The vertex entry point's module with its Position output captured (out); false (and why) when it is not one this can patch:
/// not SPIR-V, no vertex entry point, no Position output, already capturing
inline bool patch(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::string* why = nullptr) {
    const auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (in.size() < 5 || in[0] != kMagic) return fail("not SPIR-V");
    // pass 1: where things are, and the Position output
    size_t last_capability = 0, last_entry = 0, last_mode = 0, last_annotation = 0;
    uint32_t entry = 0;
    uint32_t block = 0, member = 0, posvar = 0;   // (Position: a member of a block, or a variable decorated itself)
    bool member_form = false;
    std::vector<std::pair<uint32_t, uint32_t>> pointers;   // (output pointer type id, pointee)
    std::vector<std::pair<uint32_t, uint32_t>> outputs;    // (output variable id, its pointer type)
    for (size_t i = 5; i < in.size();) {
        const uint32_t n = len(in[i]), o = op(in[i]);
        if (n == 0 || i + n > in.size()) return fail("a malformed instruction");
        if (o == OpCapability) {
            if (in[i + 1] == CapabilityTransformFeedback) return fail("it already captures");
            last_capability = i + n;
        } else if (o == OpEntryPoint) {
            if (in[i + 1] == ModelVertex && !entry) entry = in[i + 2];
            last_entry = i + n;
        } else if (o == OpExecutionMode) {
            last_mode = i + n;
        } else if (annotation(o)) {
            last_annotation = i + n;
            if (o == OpDecorate && n >= 4 && in[i + 2] == DecorationBuiltIn && in[i + 3] == BuiltInPosition) posvar = in[i + 1];
            if (o == OpMemberDecorate && n >= 5 && in[i + 3] == DecorationBuiltIn && in[i + 4] == BuiltInPosition)
                block = in[i + 1], member = in[i + 2], member_form = true;
            if ((o == OpDecorate && n >= 3 && (in[i + 2] == DecorationXfbBuffer || in[i + 2] == DecorationXfbStride)) ||
                (o == OpMemberDecorate && n >= 4 && (in[i + 3] == DecorationXfbBuffer || in[i + 3] == DecorationXfbStride)))
                return fail("it already captures");
        } else if (o == OpTypePointer && n == 4 && in[i + 2] == StorageOutput) {
            pointers.push_back({in[i + 1], in[i + 3]});
        } else if (o == OpVariable && n >= 4 && in[i + 3] == StorageOutput) {
            outputs.push_back({in[i + 2], in[i + 1]});
        }
        i += n;
    }
    if (!entry) return fail("no vertex entry point");
    if (member_form) {   // (the output variable of the block's type)
        uint32_t pointer = 0;
        for (const auto& [ptr, pointee] : pointers)
            if (pointee == block) pointer = ptr;
        for (const auto& [var, type] : outputs)
            if (pointer && type == pointer) posvar = var;
        if (!posvar) return fail("no output variable of the Position block");
    }
    if (!posvar) return fail("no Position output");
    if (!last_capability || !last_entry || !last_annotation) return fail("an unexpected module layout");
    // pass 2: the words, with the new instructions where their sections end
    const size_t after_modes = last_mode > last_entry ? last_mode : last_entry;
    out.clear();
    out.reserve(in.size() + 20);
    for (size_t i = 0; i < in.size();) {
        const size_t n = i < 5 ? 1 : len(in[i]);
        out.insert(out.end(), in.begin() + i, in.begin() + i + n);
        i += n;
        if (i == last_capability) out.insert(out.end(), {2u << 16 | OpCapability, CapabilityTransformFeedback});
        if (i == after_modes) out.insert(out.end(), {3u << 16 | OpExecutionMode, entry, ModeXfb});
        if (i == last_annotation) {
            out.insert(out.end(), {4u << 16 | OpDecorate, posvar, DecorationXfbBuffer, 0u});
            out.insert(out.end(), {4u << 16 | OpDecorate, posvar, DecorationXfbStride, 16u});
            if (member_form) out.insert(out.end(), {5u << 16 | OpMemberDecorate, block, member, DecorationOffset, 0u});
            else out.insert(out.end(), {4u << 16 | OpDecorate, posvar, DecorationOffset, 0u});
        }
    }
    return true;
}

} // namespace spirv_xfb

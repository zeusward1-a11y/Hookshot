#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// (hookshot) DLSS's captured motion (vk_draw.cpp dlss_capture_pass): a draw's triangles as the game's own vertex shader placed
// them this frame and the frame before, recorded by transform feedback (spirv_xfb.h), drawn again: each vertex's clip position
// now and then, read by gl_VertexIndex from the captures buffer; dlss_actor.frag, unchanged, writes the motion where the depth
// is the scene's. The positions are the game's clip space as its vertex stage wrote them (y up, the depth converted from NVN's
// -1..1 as the scene's): drawn with the scene's viewport (negative height, the jitter's offset), the depths meet the scene's.
// A triangle whose motion of its own (where it was, less where the camera's motion alone takes it) is over the head's limit in
// render pixels is a record of another draw or a jump (a draw matched with another one, a level of detail swapped): its
// "before" is zero, which dlss_actor.frag discards, and the camera's motion stays (09/11: whole objects with wild vectors)
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;    // (the head: dlss_actor.frag's 36 words, then the
                                                                       // clip-to-previous-clip, row major, and the limit)
layout(std430, binding = 2) readonly buffer Captured { vec4 p[]; } captured;
layout(push_constant) uniform P {   // (dlss_actor::Push's layout, which dlss_actor.frag reads tolerance from)
    uint64_t vertices;   // (unused here)
    uint64_t indices;    // (unused here)
    uint64_t origins[2]; // (unused here)
    uint64_t bones[2];   // (unused here)
    uint nbones;         // (unused here)
    uint stride;         // (unused here)
    uint count;          // (the draw's captured vertices: three a triangle)
    uint first;          // (this frame's first in the captures buffer)
    uint itype;          // (unused here)
    int base_vertex;     // (the frame before's first in the captures buffer)
    uint pos, sets;          // (unused here)
    uint joints[2], weights[2];   // (unused here)
    uint formats;            // (unused here)
    uint tolerance;          // (the fragment stage's)
} pc;
layout(location = 0) out vec4 before;

float f(uint i) { return uintBitsToFloat(data.w[i]); }
vec4 f4(uint i) { return vec4(f(i), f(i + 1u), f(i + 2u), f(i + 3u)); }

/// The vertex's motion of its own, over the limit: where it was against where the camera's motion takes where it is
bool jumped(vec4 now, vec4 then, mat4 to_prev, vec2 size, float limit) {
    const vec4 cam = to_prev * vec4(now.xy, now.z * 2.0 - now.w, now.w);   // (the depth back to NVN's: the cameras' clip)
    if (cam.w <= 1e-6 || then.w <= 1e-6) return false;
    const vec2 own = (then.xy / then.w - cam.xy / cam.w) * 0.5 * size;
    return any(greaterThan(abs(own), vec2(limit)));
}

void main() {
    const uint i = uint(gl_VertexIndex);
    if (i >= pc.count) {   // (past the draw's vertices: its triangle dropped)
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
        before = vec4(0.0);
        return;
    }
    const uint now = pc.first + i, then = uint(pc.base_vertex) + i;
    gl_Position = captured.p[now];
    const mat4 to_prev = transpose(mat4(f4(36u), f4(40u), f4(44u), f4(48u)));
    const vec2 size = vec2(f(34u), f(35u));
    const float limit = f(52u);
    const uint t = i - i % 3u;   // (the triangle's first vertex: all three test the same three, so they agree)
    bool odd = false;
    for (uint k = 0u; k < 3u && t + k < pc.count; k++)
        odd = odd || jumped(captured.p[pc.first + t + k], captured.p[uint(pc.base_vertex) + t + k], to_prev, size, limit);
    before = odd ? vec4(0.0) : captured.p[then];
}

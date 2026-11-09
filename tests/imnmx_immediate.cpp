// Independently authored fields from the pinned Mesa NAK encoder, never a shader dump.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;
using Shader::Ampere::LoweredProgram;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
static void field(Instruction& i, unsigned bit, unsigned width, uint32_t value) {
    CHECK(width <= 32 && (width == 32 || value < (uint64_t{1} << width)));
    for (unsigned n = 0; n < width; ++n) {
        const uint32_t mask = uint32_t{1} << ((bit+n)%32);
        auto& word = i.words[(bit+n)/32];
        word = (word & ~mask) | (((value >> n) & 1) ? mask : 0);
    }
}
static Instruction op(uint32_t code, uint32_t pred = 7) {
    Instruction i{}; field(i,0,12,code); field(i,12,4,pred); return i;
}
static Instruction exit_op() { auto i=op(0x94d); field(i,87,3,7); return i; }
static Instruction imnmx(uint32_t value, bool sign, bool minimum, uint32_t d=13, uint32_t a=9, uint32_t pred=7) {
    auto i=op(0x817,pred); field(i,16,8,d); field(i,24,8,a); field(i,32,32,value);
    field(i,73,1,sign); field(i,87,3,7); field(i,90,1,!minimum); return i;
}
static LoweredProgram lower(Instruction i) { const std::array code{i,exit_op()}; return Shader::Ampere::LowerToMaxwell(code); }
static bool wide(uint32_t value) { return value > 0x7ffffU && value < 0xfff80000U; }
static void lowering_checks() {
    for (uint32_t value : {0U,0x7ffffU,0x80000U,0xfff7ffffU,0xfff80000U,~0U,0x123456faU,0xc00000faU}) {
        const auto r=lower(imnmx(value,true,true)); CHECK(r);
        if (wide(value)) {
            CHECK(r.instructions.size() == 3);
            CHECK((r.instructions[0] >> 52) == 0x10 && uint32_t(r.instructions[0] >> 20) == value);
            CHECK((r.instructions[0] & 255) == 251 && ((r.instructions[0] >> 16) & 15) == 7);
            CHECK(((r.instructions[1] >> 48) & 0xfffe) == 0x5c20);
            CHECK(((r.instructions[1] >> 20) & 255) == 251);
        } else {
            // Only ordinary, low-register fast paths promise byte-identical old encoding.
            const uint64_t expected = 0x3820000000000000ULL | (uint64_t{7} << 16) | 13 |
                (uint64_t{9} << 8) | (uint64_t{value & 0x7ffffU} << 20) |
                (uint64_t{value >> 31} << 56) | (uint64_t{7} << 39) | (uint64_t{1} << 48);
            CHECK(r.instructions.size() == 2 && r.instructions[0] == expected);
        }
        for (const auto [d,a] : {std::pair{251U,9U},std::pair{13U,251U},std::pair{251U,251U}}) {
            const auto high=lower(imnmx(value,false,false,d,a)); CHECK(high && high.register_map_words);
        }
    }
    for (uint32_t pred : {2U,10U}) {
        const auto r=lower(imnmx(0xc00000fa,true,false,13,9,pred)); CHECK(r);
        for (auto word:r.instructions) CHECK(((word>>16)&15)==pred || word==r.instructions.back());
    }
    const auto disabled=lower(imnmx(0xc00000fa,true,true,13,9,15)); CHECK(disabled);
    const std::array only_exit{exit_op()};
    CHECK(disabled.instructions==Shader::Ampere::LowerToMaxwell(only_exit).instructions);
    for (unsigned bit : {64U,72U,74U,76U,77U,81U,84U,91U}) {
        auto i=imnmx(0x12345678,false,true); field(i,bit,1,1); CHECK(!lower(i));
    }
    auto selector=imnmx(0x12345678,false,true); field(selector,87,3,3); CHECK(!lower(selector));
    auto cbuf=imnmx(0,false,true); field(cbuf,0,12,0xa17); field(cbuf,54,5,31); CHECK(!lower(cbuf));
    std::puts("PASS: full literal, ordinary signed20 encoding, high aliases, predicate and refusal controls");
}
static void existing_controls(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::vector<Instruction> cases;
    for (auto value : {0U,0x7ffffU,0xfff80000U,~0U}) cases.push_back(imnmx(value,true,true));
    auto reg=imnmx(3,false,false); field(reg,0,12,0x217); cases.push_back(reg);
    auto cbuf=imnmx(0,true,true); field(cbuf,0,12,0xa17); field(cbuf,40,14,0x10); field(cbuf,54,5,3); cases.push_back(cbuf);
    auto uniform=imnmx(63,false,true); field(uniform,0,12,0xc17); field(uniform,91,1,1); cases.push_back(uniform);
    for (size_t n=0;n<cases.size();++n) {
        auto r=lower(cases[n]); CHECK(r);
        std::ofstream out(directory/("control-"+std::to_string(n)+".bin"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(r.instructions.data()),r.instructions.size()*sizeof(uint64_t)); CHECK(out.good());
    }
}

#ifndef IMNMX_LOWERING_ONLY
#include <map>
#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

struct Environment final : Shader::Environment {
    std::vector<uint64_t> code;
    explicit Environment(const LoweredProgram& lowered) {
        stage=Shader::Stage::Compute; code.resize(0x50/8);
        for (auto word:lowered.instructions) { if (code.size()%4==0) code.push_back(0); code.push_back(word); }
    }
    uint64_t ReadInstruction(uint32_t at) override { CHECK(at/8<code.size()); return code[at/8]; }
    uint32_t ReadCbufValue(uint32_t,uint32_t) override { std::abort(); }
    Shader::TextureType ReadTextureType(uint32_t) override { std::abort(); }
    Shader::TexturePixelFormat ReadTexturePixelFormat(uint32_t) override { std::abort(); }
    bool IsTexturePixelFormatInteger(uint32_t) override { std::abort(); }
    uint32_t ReadViewportTransformState() override { return 0; }
    uint32_t TextureBoundBuffer() const override { return 0; }
    uint32_t LocalMemorySize() const override { return 0; }
    uint32_t SharedMemorySize() const override { return 16; }
    std::array<uint32_t,3> WorkgroupSize() const override { return {32,1,1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(uint32_t,uint32_t) override { return std::nullopt; }
    void Dump(uint64_t,uint64_t) override { std::abort(); }
};
static Instruction move(uint32_t reg,uint32_t value) {
    auto i=op(0x802);field(i,16,8,reg);field(i,32,32,value);field(i,72,4,15);return i;
}
static Instruction tid(uint32_t reg) {
    auto i=op(0x919);field(i,16,8,reg);field(i,72,8,33);return i;
}
static Instruction store(uint32_t reg,uint32_t offset) {
    auto i=op(0x388);field(i,24,8,255);field(i,32,8,reg);field(i,40,24,offset);field(i,73,3,4);return i;
}
static bool reaches(Shader::IR::Value value, Shader::IR::Inst* target, std::set<Shader::IR::Inst*>& visited) {
    auto* inst=value.TryInstRecursive();if (!inst) return false;
    if (inst==target) return true;if (!visited.insert(inst).second) return false;
    for (size_t n=0;n<inst->NumArgs();++n) if (reaches(inst->Arg(n),target,visited)) return true;
    return false;
}
static bool literal_flow(Shader::IR::Value value,uint32_t literal,std::set<Shader::IR::Inst*>& visited) {
    if (value.IsImmediate()) return value.Type()==Shader::IR::Type::U32 && value.U32()==literal;
    auto* inst=value.TryInstRecursive();if (!inst || !visited.insert(inst).second) return false;
    // A predicated MOV32I can reach the second predicated instruction through an SSA phi.
    // Only transparent merges may carry its literal, never an arithmetic transformation.
    const auto op=inst->GetOpcode();
    if (op!=Shader::IR::Opcode::Phi && op!=Shader::IR::Opcode::Identity && op!=Shader::IR::Opcode::SelectU32) return false;
    for (size_t n=op==Shader::IR::Opcode::SelectU32?1:0;n<inst->NumArgs();++n)
        if (literal_flow(inst->Arg(n),literal,visited)) return true;
    return false;
}
static bool thread_input(Shader::IR::Value value,std::set<Shader::IR::Inst*>& visited) {
    auto* inst=value.TryInstRecursive();if (!inst || !visited.insert(inst).second) return false;
    if (inst->GetOpcode()==Shader::IR::Opcode::LocalInvocationId) return true;
    for (size_t n=0;n<inst->NumArgs();++n) if (thread_input(inst->Arg(n),visited)) return true;
    return false;
}
struct Case {
    uint32_t literal, input=42, d=13, a=9, pred=7;
    bool sign=false, minimum=true, dynamic=false, uniform=false;
};
static unsigned modules=0;
static void pipeline(const std::filesystem::path& directory, const Case& c) {
    constexpr uint32_t sentinel=0x11223344, uniform_value=0x55667788;
    std::vector<Instruction> source;
    if (c.uniform) { auto i=op(0x882);field(i,32,32,uniform_value);source.push_back(i); }
    source.push_back(move(c.d,99));source.push_back(c.dynamic?tid(c.a):move(c.a,c.input));
    source.push_back(move(250,sentinel));
    if (c.pred==2 || c.pred==10) {
        source.push_back(tid(1));
        auto compare=op(0x20c);field(compare,24,8,1);field(compare,32,8,255);field(compare,76,3,5);
        field(compare,81,3,2);field(compare,84,3,7);field(compare,87,3,7);source.push_back(compare);
    }
    source.push_back(imnmx(c.literal,c.sign,c.minimum,c.d,c.a,c.pred));
    source.push_back(store(c.d,0));source.push_back(store(250,4));
    if (c.uniform) { auto i=op(0xc02);field(i,16,8,20);field(i,72,4,15);field(i,91,1,1);source.push_back(i);source.push_back(store(20,8)); }
    source.push_back(exit_op());
    const auto lowered=Shader::Ampere::LowerToMaxwell(source);
    if (!lowered) std::fprintf(stderr,"IMNMX: %.*s at+%u\n",int(lowered.error.size()),lowered.error.data(),lowered.error_offset);
    CHECK(lowered); CHECK(lowered.register_map_words); // At least the sentinel's native R250 is renamed.
    Environment env(lowered);
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::Maxwell::Flow::CFG cfg{env,flow_blocks,0x50};
    Shader::HostTranslateInfo host{};host.support_int64=host.support_float64=host.support_float16=true;
    host.support_conditional_barrier=true;
    auto program=Shader::Maxwell::TranslateProgram(insts,blocks,env,cfg,host);
    const auto expected_op=c.sign?(c.minimum?Shader::IR::Opcode::SMin32:Shader::IR::Opcode::SMax32)
                                 :(c.minimum?Shader::IR::Opcode::UMin32:Shader::IR::Opcode::UMax32);
    std::vector<Shader::IR::Inst*> calculations;
    Shader::IR::Inst* result_store=nullptr;
    bool sentinel_ok=false, uniform_ok=!c.uniform;
    for (auto* block:program.blocks) for (auto& inst:block->Instructions()) {
        if (inst.GetOpcode()==expected_op) {
            std::set<Shader::IR::Inst*> left_seen,right_seen;
            const bool left=literal_flow(inst.Arg(0),c.literal,left_seen),right=literal_flow(inst.Arg(1),c.literal,right_seen);
            CHECK(left || right);
            if (c.dynamic) {std::set<Shader::IR::Inst*> seen;CHECK(thread_input(inst.Arg(left?1:0),seen));}
            calculations.push_back(&inst);
        }
        if (inst.GetOpcode()==Shader::IR::Opcode::WriteSharedU32) {
            CHECK(inst.Arg(0).IsImmediate());const uint32_t offset=inst.Arg(0).U32();
            if (offset==0) result_store=&inst;
            if (offset==4) sentinel_ok=inst.Arg(1).IsImmediate() && inst.Arg(1).U32()==sentinel;
            if (offset==8) uniform_ok=inst.Arg(1).IsImmediate() && inst.Arg(1).U32()==uniform_value;
        }
    }
    CHECK(result_store && sentinel_ok && uniform_ok);
    if (!result_store->Arg(1).IsImmediate()) {
        CHECK(c.pred!=15);
        CHECK(!calculations.empty());bool used=false;
        for (auto* calculation:calculations) {
            if (!c.dynamic) {
                CHECK(calculation->Arg(0).IsImmediate() && calculation->Arg(1).IsImmediate());
                const auto a=calculation->Arg(0).U32(), b=calculation->Arg(1).U32();
                CHECK((a==c.input && b==c.literal) || (a==c.literal && b==c.input));
            }
            std::set<Shader::IR::Inst*> visited; used|=reaches(result_store->Arg(1),calculation,visited);
        }
        CHECK(used); // Actual computed min/max, through a phi if predicated, reaches the observable store.
    } else {
        CHECK(!c.dynamic);
        uint32_t expected;
        if (c.pred==15) expected=c.d==c.a?c.input:99;
        else if (c.sign) {
            const int32_t a=std::bit_cast<int32_t>(c.input),b=std::bit_cast<int32_t>(c.literal);
            expected=std::bit_cast<uint32_t>(c.minimum?std::min(a,b):std::max(a,b));
        } else expected=c.minimum?std::min(c.input,c.literal):std::max(c.input,c.literal);
        CHECK(result_store->Arg(1).IsImmediate() && result_store->Arg(1).U32()==expected);
    }
    Shader::Profile profile{};profile.supported_spirv=0x00010600;profile.support_int64=true;
    const auto spirv=Shader::Backend::SPIRV::EmitSPIRV(profile,program);
    CHECK(spirv.size()>5);unsigned stores=0;
    for (size_t at=5;at<spirv.size();) {const uint32_t n=spirv[at]>>16;CHECK(n && at+n<=spirv.size());
        if (static_cast<spv::Op>(spirv[at]&0xffff)==spv::Op::OpStore) ++stores;at+=n;}
    CHECK(stores>=2);
    std::filesystem::create_directories(directory);
    std::ofstream out(directory/("case-"+std::to_string(modules++)+".spv"),std::ios::binary);
    out.write(reinterpret_cast<const char*>(spirv.data()),spirv.size()*sizeof(uint32_t));CHECK(out.good());
}
#endif

int main(int argc,char** argv) {
    if (argc==2 && std::string(argv[1])=="--expect-original-refusal") {
        auto r=lower(imnmx(0x12345678,true,true));CHECK(!r && r.error=="native integer min/max mode" && r.error_offset==0);
        std::puts("PASS: original source refuses the authored wide immediate");return 0;
    }
    if (argc==3 && std::string(argv[1])=="--existing-controls") {existing_controls(argv[2]);return 0;}
    lowering_checks();
#ifndef IMNMX_LOWERING_ONLY
    CHECK(argc==2);const std::filesystem::path directory{argv[1]};
    for (auto value:{0x7ffffU,0x80000U,0xfff7ffffU,0xfff80000U,0x7fffffffU,0x80000000U,0xc00000faU,~0U})
        for (bool sign:{false,true}) for (bool minimum:{false,true}) pipeline(directory,{value,42,13,9,7,sign,minimum});
    for (const auto [d,a]:{std::pair{251U,9U},std::pair{13U,251U},std::pair{251U,251U}})
        for (auto value:{0x7ffffU,0xfff80000U,0x123456faU,0xc00000faU}) pipeline(directory,{value,0x80000001U,d,a,7,true,false,false,true});
    for (bool sign:{false,true}) for (bool minimum:{false,true})
        for (uint32_t pred:{7U,2U,10U}) pipeline(directory,{0xc00000faU,42,13,9,pred,sign,minimum,true});
    pipeline(directory,{0xc00000faU,42,13,9,15,true,true});
    CHECK(modules==57);std::printf("PASS: %u authored full lowering/REGMAP/CFG/IR/SPIR-V modules\n",modules);
#endif
}

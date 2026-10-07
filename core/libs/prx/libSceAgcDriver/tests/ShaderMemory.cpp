#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "Optimization/SrtWalker/SrtEvaluator.hpp"
#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "SpirvBackend/SpirvAnalysis.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include "CacheKey.hpp"
#include "BdaAbi.hpp"
#include "ImageTableAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <future>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void expectFailure(TAction action, const char* expected, const char* message) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, "unexpected failure reason");
        return;
    }
    throw std::runtime_error(message);
}

void verifyResult(const ShaderRecompiler::RecompileResult& first, const ShaderRecompiler::RecompileResult& second) {
    require(first.spirv == second.spirv, "replayed SPIR-V differs");
    require(first.pushConstants == second.pushConstants, "replayed push constants differ");
    require(first.bdaAbiVersion == second.bdaAbiVersion && first.bindings.size() == second.bindings.size(), "replayed layout differs");
    for (std::size_t index = 0; index < first.bindings.size(); ++index) {
        const auto& left = first.bindings[index];
        const auto& right = second.bindings[index];
        require(left.kind == right.kind && left.role == right.role && left.descriptorSet == right.descriptorSet && left.binding == right.binding && left.count == right.count && left.guestDescriptor == right.guestDescriptor && left.readOnly == right.readOnly, "replayed binding differs");
    }
}

void verifyRegisterSources() {
    using namespace ShaderRecompiler;
    IrResourcePlan plan;
    IrValue samplerRegister(IrOpcode::Void, IrType::ScalarReg, 0);
    IrValue bufferRegister(IrOpcode::Void, IrType::ScalarReg, 1);
    IrValue sameSamplerRegister(IrOpcode::Void, IrType::ScalarReg, 2);
    samplerRegister.SetRegister({RegisterBank::Scalar, 8});
    bufferRegister.SetRegister({RegisterBank::Scalar, 12});
    sameSamplerRegister.SetRegister({RegisterBank::Scalar, 8});
    IrValue samplerRead(IrOpcode::GetUserData, IrType::U32, 3);
    IrValue bufferRead(IrOpcode::GetUserData, IrType::U32, 4);
    IrValue sameSamplerRead(IrOpcode::GetUserData, IrType::U32, 5);
    samplerRead.AddArgument(&samplerRegister);
    bufferRead.AddArgument(&bufferRegister);
    sameSamplerRead.AddArgument(&sameSamplerRegister);
    require(!EquivalentValue(plan, &samplerRead, &bufferRead), "sampler SGPRs were merged with buffer SGPRs");
    require(EquivalentValue(plan, &samplerRead, &sameSamplerRead), "identical user data reads were not recognized");
    sameSamplerRegister.SetRegister({RegisterBank::UserData, 8});
    require(!EquivalentValue(plan, &samplerRead, &sameSamplerRead), "different register banks were merged");
    IrValue firstVector(IrOpcode::Void, IrType::VectorReg, 6);
    IrValue secondVector(IrOpcode::Void, IrType::VectorReg, 7);
    firstVector.SetRegister({RegisterBank::Vector, 0});
    secondVector.SetRegister({RegisterBank::Vector, 1});
    require(!EquivalentValue(plan, &firstVector, &secondVector), "different vector registers were merged");
    require(!EquivalentValue(plan, &samplerRegister, &firstVector), "different register types were merged");
}

void verifyEvaluatedValues() {
    using namespace ShaderRecompiler;
    std::vector<std::unique_ptr<IrValue>> values;
    for (std::uint32_t id = 0; id < 1000u; id++) {
        values.push_back(std::make_unique<IrValue>(IrOpcode::Void, IrType::U32, id));
    }
    Detail::EvaluatedValues table;
    std::uint64_t found = 0;
    require(!table.Find(values.front().get(), found), "evaluated values: an empty table found a value");
    for (std::uint32_t id = 0; id < values.size(); id++) {
        table.Insert(values[id].get(), std::uint64_t{id} * 3u);
    }
    for (std::uint32_t id = 0; id < values.size(); id++) {
        require(table.Find(values[id].get(), found) && found == std::uint64_t{id} * 3u, "evaluated values: a value was lost when the table grew");
    }
    table.Insert(values[7].get(), 0u);
    require(table.Find(values[7].get(), found) && found == 21u, "evaluated values: a second insert replaced the first value");
    IrValue absent(IrOpcode::Void, IrType::U32, 1000u);
    require(!table.Find(&absent, found), "evaluated values: a value that was never inserted was found");
}

// The pure flat slots of a hand-built plan (Detail::ComputePureFlatSlots): a slot is pure unless
// a descriptor dword, a condition, a uniform value or another slot's address cone reaches it.
void verifyPureFlatSlots() {
    using namespace ShaderRecompiler;
    std::vector<std::unique_ptr<IrValue>> values;
    std::uint32_t ids = 0;
    const auto make = [&](IrOpcode opcode, IrType type) -> IrValue& {
        values.push_back(std::make_unique<IrValue>(opcode, type, ids++));
        return *values.back();
    };
    const auto constant = [&](std::uint32_t value) -> IrValue& {
        auto& immediate = make(IrOpcode::Void, IrType::U32);
        immediate.SetImmediateU32(value);
        return immediate;
    };
    auto& resource = make(IrOpcode::GetSrtResource, IrType::SrtResource);
    const auto userData = [&](std::uint32_t index) -> IrValue& {
        auto& reg = make(IrOpcode::Void, IrType::ScalarReg);
        reg.SetRegister({RegisterBank::Scalar, index});
        auto& read = make(IrOpcode::GetUserData, IrType::U32);
        read.AddArgument(&reg);
        return read;
    };
    const auto handle = [&](IrValue& low, IrValue& high) -> IrValue& {
        auto& composed = make(IrOpcode::CompositeConstructU64, IrType::U64);
        composed.AddArgument(&low);
        composed.AddArgument(&high);
        return composed;
    };
    const auto rawRead = [&](IrValue& address, std::uint32_t offset) -> IrValue& {
        auto& read = make(IrOpcode::LoadAddressU32, IrType::U32);
        read.AddArgument(&address);
        read.AddArgument(&constant(offset));
        return read;
    };
    const auto readConst = [&](std::uint32_t slot) -> IrValue& {
        auto& read = make(IrOpcode::ReadConst, IrType::U32);
        read.AddArgument(&resource);
        read.AddArgument(&constant(slot));
        return read;
    };
    // Slots: A (0) reads through a pointer B (1) read from user data; C (2) and D (3) read from
    // user-data handles; a descriptor source consumes C.
    auto& b = rawRead(handle(userData(0), userData(1)), 0);
    auto& c = rawRead(handle(userData(2), userData(3)), 4);
    auto& a = rawRead(handle(readConst(1), userData(4)), 8);
    auto& d = rawRead(handle(userData(5), userData(6)), 12);
    IrResourcePlan plan;
    plan.srtPlanComplete = true;
    plan.resourceTrackingComplete = true;
    plan.srtReads = {{&a, 0}, {&b, 1}, {&c, 2}, {&d, 3}};
    DescriptorSource source;
    source.dwordCount = 1;
    source.dwords[0] = &readConst(2);
    plan.descriptorSources.push_back(source);
    using Pure = std::vector<std::uint8_t>;
    require(Detail::ComputePureFlatSlots(plan) == Pure{1, 0, 0, 1}, "pure flat slots: the address cone or the descriptor source was not excluded");
    plan.controlFlow.push_back({&readConst(3), {}, {}});
    require(Detail::ComputePureFlatSlots(plan) == Pure{1, 0, 0, 0}, "pure flat slots: a control-flow condition was not excluded");
    plan.controlFlow.clear();
    auto& phi = make(IrOpcode::Phi, IrType::U32);
    phi.AddArgument(&readConst(0));
    phi.AddArgument(&constant(0));
    plan.descriptorSources[0].dwords[0] = &phi;
    require(Detail::ComputePureFlatSlots(plan) == Pure{0, 0, 1, 1}, "pure flat slots: a phi argument was not excluded");
    plan.descriptorSources[0].dwords[0] = &b;
    require(Detail::ComputePureFlatSlots(plan) == Pure{1, 0, 1, 1}, "pure flat slots: a raw read named directly was not excluded");
    plan.descriptorSources[0].dwords[0] = &readConst(2);
    plan.uniformFill.fill.kind = UniformFillKind::Buffer;
    plan.uniformFill.fill.words = 1;
    plan.uniformFill.values[0] = &readConst(3);
    require(Detail::ComputePureFlatSlots(plan) == Pure{1, 0, 0, 0}, "pure flat slots: a uniform-fill value was not excluded");
    plan.uniformFill = {};
    plan.descriptorSources[0].tableColumn = TableColumn{};
    require(Detail::ComputePureFlatSlots(plan) == Pure{0, 0, 0, 0}, "pure flat slots: an image table column did not disqualify the plan");
    plan.descriptorSources[0].tableColumn.reset();
    plan.requiresSpecializationMemory = true;
    require(Detail::ComputePureFlatSlots(plan) == Pure{0, 0, 0, 0}, "pure flat slots: specialization memory did not disqualify the plan");
    plan.requiresSpecializationMemory = false;
    plan.srtPlanComplete = false;
    require(Detail::ComputePureFlatSlots(plan) == Pure{0, 0, 0, 0}, "pure flat slots: an incomplete plan was classified");
}

struct alignas(256) TableTexture {
    std::array<std::uint8_t, 256> bytes{};
};

std::array<std::uint32_t, 4> TableT(const void* base, std::uint32_t format, std::uint32_t type) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (type << 28u)};
}

std::array<std::uint32_t, 4> TableV(const void* base, std::uint32_t stride, std::uint32_t records) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, 0xfacu};
}

bool TableCovered(const std::vector<ShaderRecompiler::MemoryRegion>& regions, const void* pointer, std::size_t bytes) {
    auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
    const auto end = address + bytes;
    while (address < end) {
        const auto region = std::find_if(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& candidate) { return address >= candidate.guestAddress && address < candidate.guestAddress + candidate.bytes.size(); });
        if (region == regions.end()) return false;
        address = region->guestAddress + region->bytes.size();
    }
    return true;
}

bool TableTouched(const std::vector<ShaderRecompiler::MemoryRegion>& regions, const void* pointer, std::size_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
    return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return region.guestAddress < address + bytes && address < region.guestAddress + region.bytes.size(); });
}

struct TableRequest {
    std::vector<std::uint32_t> code;
    std::array<std::uint32_t, 2> userData{};
    std::array<std::uint32_t, 3> capabilities{29u, 5302u, 1u};
    std::array<std::string_view, 1> extensions{"SPV_EXT_descriptor_indexing"};
    ShaderRecompiler::RecompileRequest request{};

    TableRequest(std::vector<std::uint32_t> words, const void* srt, bool groupId = false) : code(std::move(words)) {
        using namespace ShaderRecompiler;
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(srt));
        userData = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
        request.shader = {ShaderStage::Compute, 0x40000u + static_cast<std::uint64_t>(code.size()) * 4u + (groupId ? 0x1000u : 0u), code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{groupId ? 8u : 64u, groupId ? 8u : 1u, 1u}, 0u, {groupId, false, false}, false, groupId ? 2u : 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.bdaAbiVersion = BdaAbi::Version;
        request.target.supportedCapabilities = capabilities;
        request.target.supportedExtensions = extensions;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;
    }
};

const ShaderRecompiler::TableColumn& ColumnOf(const ShaderRecompiler::IrResourcePlan& plan, std::uint32_t view) {
    const auto& column = plan.descriptorSources.at(plan.info.tableViews.at(view).source).tableColumn;
    require(column.has_value(), "image table: a view has no column");
    return *column;
}

std::uint32_t ViewWord(const ShaderRecompiler::ImageTableSnapshot& tables, std::uint32_t view, std::uint32_t field) {
    return tables.map.at(ShaderRecompiler::ImageTableAbi::HeaderWords + ShaderRecompiler::ImageTableAbi::ViewWords * view + field);
}

std::vector<std::uint32_t> ViewCodes(const ShaderRecompiler::ImageTableSnapshot& tables, std::uint32_t view) {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    const auto start = ViewWord(tables, view, Abi::ViewMapStart);
    const auto keys = ViewWord(tables, view, Abi::ViewKeyCount);
    return std::vector<std::uint32_t>(tables.map.begin() + start, tables.map.begin() + start + keys);
}

void verifyScalarBufferRule() {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    require(!Abi::ScalarBufferDword(0xfffffff0u, 0x20u, 0x200000000ull).has_value(), "scalar buffer rule: a carry past 2^32 was in range");
    require(Abi::ScalarBufferDword(0xfffffff0u, 0x0cu, 0x200000000ull) == 0xfffffffcu, "scalar buffer rule: the last dword below 2^32 was not in range");
    require(Abi::BufferBytes(16u, 0x10000000u) == 0x100000000ull && Abi::BufferBytes(0u, 12u) == 12u, "scalar buffer rule: stride times records is wrong");
    require(!Abi::ScalarBufferDword(0u, 8u, 8u).has_value() && Abi::ScalarBufferDword(0u, 4u, 8u) == 4u && Abi::ScalarBufferDword(0u, 7u, 8u) == 4u && Abi::ScalarBufferDword(1u, 2u, 4u) == 0u, "scalar buffer rule: the num_records edge is wrong");
    for (const std::uint64_t size : {0ull, 7ull, 8ull, 408ull, 409ull, 0x100000000ull, 0x200000000ull}) {
        for (const std::uint32_t addend : {0u, 8u, 0xfffffff0u}) {
            for (const std::uint32_t immediate : {0u, 16u, 0x88u}) {
                std::uint32_t brute = 0;
                for (std::uint64_t record = 0; record < 1000u; record++) {
                    const auto offset = static_cast<std::uint64_t>(addend) + record * 48u;
                    if (offset > 0xffffffffull) break;
                    if (Abi::ScalarBufferDword(static_cast<std::uint32_t>(offset), immediate, size).has_value()) brute++;
                    else break;
                }
                const auto closed = Abi::ScalarBufferRecords(addend, 48u, immediate, size);
                require(closed == brute || (closed > 1000u && brute == 1000u), "scalar buffer rule: the closed-form record count disagrees with the per-record rule");
            }
        }
    }
}

void verifyScalarAddressRule() {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    require(Abi::ScalarAddressDword(0x1000u, 0x23u, 0x41u) == 0x1060u && Abi::ScalarAddressDword(0x1001u, 0x20u, 0u) == 0x1021u, "scalar address rule: the offset and immediate are not dword-aligned on their own");
    require(Abi::ScalarAddressDword(0x1000u, 0x20u, 0xfffffff0u) == 0x1010u && !Abi::ScalarAddressDword(0x8u, 0x4u, 0xfffffff0u).has_value(), "scalar address rule: a negative immediate is wrong");
    require(!Abi::ScalarAddressDword(0xfffffffffffffff0ull, 0x10u, 0u).has_value() && !Abi::ScalarAddressDword(0xfffffffffffffff0ull, 0x8u, 0x8u).has_value() && Abi::ScalarAddressDword(0xfffffffffffffff0ull, 0x8u, 0x4u) == 0xfffffffffffffffcull, "scalar address rule: a carry past 2^64 is wrong");
    for (const std::uint32_t stride : {4u, 32u, 48u, 0x330u}) {
        const std::uint64_t records = Abi::ScalarAddressRecords(stride);
        require((records - 1u) * stride <= 0xffffffffull && records * stride > 0xffffffffull, "scalar address rule: the record count does not end at 2^32");
        for (const std::uint32_t maxKey : {0u, 0xffu, 0xffffu, 0x3fffffffu, 0x40000000u, 0xffffffffu}) {
            const bool exact = static_cast<std::uint64_t>(maxKey) * stride <= 0xffffffffull;
            require(Abi::KeyRecords(maxKey, stride) == (exact ? maxKey + 1u : 0xffffffffu), "scalar address rule: the key bound is wrong");
        }
    }
}

void verifyImageTablePlans() {
    using namespace ShaderRecompiler;
    static std::array<std::uint32_t, 16> srt{};
    const auto plan = [&](std::vector<std::uint32_t> code, bool groupId = false) {
        TableRequest table(std::move(code), srt.data(), groupId);
        return GetResourcePlan(table.request);
    };
    const auto decal = plan({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u,
        0x8711ff02u, 0xfffffff1u, 0xf4240482u, 0x22000008u, 0x7e080280u, 0xbf041312u, 0xbf840015u, 0x8f148212u,
        0xf4200544u, 0x28000000u, 0x9316ff15u, 0x00000330u, 0xf42c0806u, 0x2c000088u, 0xf42005c6u, 0x2c000080u,
        0xf09c8f08u, 0x01090000u, 0x06080104u, 0xbf0d8917u, 0xbf840005u, 0xf4280a06u, 0x2c000120u, 0xf09c8208u,
        0x010a0000u, 0x06080104u, 0x81128112u, 0xbf82ffe9u, 0xe0700000u, 0x80070400u, 0xbf810000u}, true);
    require(decal->info.tableViews.size() == 3u, "image table: the decal program does not have three views");
    const std::array<std::uint32_t, 3> offsets{0x98u, 0x88u, 0x120u};
    for (std::uint32_t view = 0; view < 3u; view++) {
        const auto& column = ColumnOf(*decal, view);
        require(column.stride == 0x330u && column.addend == 0u && column.offset == offsets[view] && column.dwordCount == 4u && column.sampler == (view == 1u), "image table: a decal column is wrong");
        require(column.keyDomain.has_value() && column.keyDomain->alignment == 4u && column.keyDomain->offset == 0u, "image table: the decal key domain is missing");
        require(decal->info.tableViews[view].sampler == (view == 1u) && (view == 1u || (decal->info.tableViews[view].r128 && decal->info.tableViews[view].dimension == RdnaImageDimension::Dim2D)), "image table: a decal view is wrong");
    }
    std::uint32_t planningOnly = 0;
    bool flagsRead = false;
    for (const auto& memory : decal->memoryInfo) {
        if (memory.kind != ResourceKind::ScalarBuffer) continue;
        const bool table = (memory.offset >= 0x88u && memory.offset < 0xa8u) || (memory.offset >= 0x120u && memory.offset < 0x130u);
        if (memory.planningOnly && table) planningOnly++;
        if (memory.offset == 0x80u) flagsRead = !memory.planningOnly;
    }
    require(planningOnly == 12u && flagsRead, "image table: the descriptor reads are not planning-only while the record field read stays");
    require(decal->info.usesFaultBuffer && decal->requiresSpecializationMemory, "image table: the decal plan has no fault buffer or specialization memory");

    const auto wide = plan({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x8f108510u,
        0xf42c0502u, 0x20000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u});
    require(wide->info.tableViews.size() == 1u && ColumnOf(*wide, 0).stride == 32u && ColumnOf(*wide, 0).dwordCount == 8u && !wide->info.tableViews[0].r128, "image table: key << 5 with an r256 T# was not planned");
    const auto affine = plan({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x93109010u,
        0x81108810u, 0xf4280502u, 0x20000000u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u});
    require(affine->info.tableViews.size() == 1u && ColumnOf(*affine, 0).stride == 16u && ColumnOf(*affine, 0).addend == 8u && ColumnOf(*affine, 0).dwordCount == 4u, "image table: key * 16 + 8 was not planned");
    const auto load = plan({0xf4080100u, 0xfa000000u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x93109010u, 0x81108810u, 0xf4280502u,
        0x20000000u, 0xf0008f08u, 0x00050000u, 0xe0700000u, 0x80070000u, 0xbf810000u});
    require(load->info.tableViews.size() == 1u && !load->info.tableViews[0].sampler && load->info.tableViews[0].r128 && load->info.tableViews[0].dimension == RdnaImageDimension::Dim2D, "image table: image_load through key * 16 + 8 has no image view");
    require(ColumnOf(*load, 0).stride == 16u && ColumnOf(*load, 0).addend == 8u && ColumnOf(*load, 0).dwordCount == 4u && !ColumnOf(*load, 0).sampler && load->info.usesFaultBuffer, "image table: image_load through key * 16 + 8 was not planned");

    const auto failing = [&](std::vector<std::uint32_t> code, const char* expected, const char* message) {
        TableRequest table(std::move(code), srt.data());
        expectFailure([&] { static_cast<void>(GetResourcePlan(table.request)); }, expected, message);
    };
    failing({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0xf4280502u,
        0x20000000u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u}, "GetImageResource dword 0 is not a valid runtime value", "image table: a raw byte offset was planned");
    failing({0xf4080100u, 0xfa000000u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x8f108510u, 0xf42c0502u, 0x20000000u,
        0xf0200f08u, 0x00050200u, 0xbf810000u}, "image table: ImageWrite", "image table: a table T# feeding image_store was planned");
    failing({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0x7e200500u, 0x8f108510u, 0xf42c0502u, 0x20000000u,
        0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80010000u, 0xbf810000u}, "stores to the buffer it loads descriptors from", "image table: a store through the table V# was planned");
    failing({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x8f108410u,
        0xf4240482u, 0x20000000u, 0xf40c0509u, 0xfa000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u,
        0xbf810000u}, "GetImageResource dword 0 is not a valid runtime value", "image table: a pointer-loaded T# was planned");

    TableRequest exec({0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x8f108510u,
        0xf42c0502u, 0x20000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u}, srt.data());
    const auto program = PrepareResourceProgram(exec.request);
    bool sampled = false;
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (inst->Opcode() != IrOpcode::ImageSampleRaw) continue;
            sampled = true;
            require(inst->ArgumentCount() == 4u && inst->Argument(3)->Type() == IrType::U1, "image table: image_sample does not carry EXEC");
        }
    }
    require(sampled, "image table: the program has no image_sample");
}

void verifyImageTableSnapshot() {
    using namespace ShaderRecompiler;
    namespace Abi = ImageTableAbi;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Format32UInt = 20;
    constexpr std::uint32_t Format16SInt = 12;
    constexpr std::uint32_t Format11_11_10UInt = 34;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Type3D = 10;
    constexpr std::uint32_t Stride = 48;
    constexpr std::uint32_t HeapBytes = 8u * Stride + 24u;
    static TableTexture textures[4];
    struct alignas(4096) Guest {
        std::array<std::uint32_t, 9 * Stride / 4> heap{};
        std::array<std::uint32_t, 16> srt{};
        std::array<std::uint32_t, 8> keys{};
        std::array<std::uint32_t, 4> output{};
    };
    static Guest guest;
    const auto record = [&](std::uint32_t index, std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), guest.heap.begin() + index * (Stride / 4u) + 4u);
    };
    record(0, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    record(1, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    record(2, TableT(textures[1].bytes.data(), Format32UInt, Type2D));
    record(3, TableT(textures[2].bytes.data(), Format16SInt, Type2D));
    record(4, {0u, 0u, 0u, 0u});
    record(5, {0x1234u, 0x5678u, 0x9abcu, 0x0000ffacu});
    record(6, TableT(textures[3].bytes.data(), Format8888UNorm, Type3D));
    record(7, TableT(textures[3].bytes.data(), Format11_11_10UInt, Type2D));
    record(8, TableT(textures[3].bytes.data(), Format8888UNorm, Type2D));
    const auto fill = [&](const void* heap, std::uint32_t bytes, std::uint32_t dword3 = 0xfacu) {
        auto heapV = TableV(heap, 0u, bytes);
        heapV[3] = dword3;
        const auto outputV = TableV(guest.output.data(), 0u, 16u);
        const auto keysV = TableV(guest.keys.data(), 0u, static_cast<std::uint32_t>(sizeof(guest.keys)));
        std::copy(heapV.begin(), heapV.end(), guest.srt.begin());
        std::fill(guest.srt.begin() + 4, guest.srt.begin() + 8, 0u);
        std::copy(keysV.begin(), keysV.end(), guest.srt.begin() + 8);
        std::copy(outputV.begin(), outputV.end(), guest.srt.begin() + 12);
    };
    fill(guest.heap.data(), HeapBytes);
    const std::vector<std::uint32_t> snapshotCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    TableRequest table(snapshotCode, guest.srt.data());
    const auto plan = GetResourcePlan(table.request);
    require(plan->info.tableViews.size() == 1u && !ColumnOf(*plan, 0).keyDomain.has_value() && ColumnOf(*plan, 0).stride == Stride && ColumnOf(*plan, 0).offset == 16u, "image table: the snapshot program's column is wrong");

    AgcDriver::ShaderMemory memory({});
    const auto capture = memory.Capture(table.request);
    const auto& tables = capture->snapshot.tables;
    std::uint32_t brute = 0;
    for (std::uint32_t index = 0; index < 1000u && Abi::ScalarBufferDword(index * Stride, 16u, HeapBytes).has_value(); index++) brute++;
    require(brute == 9u && ViewWord(tables, 0, Abi::ViewKeyCount) == brute, "image table: the record count disagrees with the scalar buffer rule");
    require(tables.map[0] == Abi::Version && tables.map[1] == 1u && tables.map[2] == 5u && ViewWord(tables, 0, Abi::ViewSizeLow) == HeapBytes && ViewWord(tables, 0, Abi::ViewSizeHigh) == 0u && ViewWord(tables, 0, Abi::ViewZeroCode) == Abi::NullCode, "image table: the map header is wrong");
    const auto heapBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest.heap.data()));
    require(ViewWord(tables, 0, Abi::ViewBaseLow) == static_cast<std::uint32_t>(heapBase) && ViewWord(tables, 0, Abi::ViewBaseHigh) == static_cast<std::uint32_t>(heapBase >> 32u), "image table: the map base is wrong");
    const std::vector<std::uint32_t> codes{Abi::ImageCode(Abi::FloatClass, 0), Abi::ImageCode(Abi::FloatClass, 0), Abi::ImageCode(Abi::UintClass, 1), Abi::ImageCode(Abi::SintClass, 2), Abi::NullCode, Abi::PoisonCode(1), Abi::PoisonCode(2), Abi::PoisonCode(3), Abi::PoisonCode(4)};
    require(ViewCodes(tables, 0) == codes, "image table: the map codes are wrong");
    require(tables.images.size() == 3u && tables.images[0].numericClass == IrTextureNumericClass::Float && tables.images[1].numericClass == IrTextureNumericClass::Uint && tables.images[2].numericClass == IrTextureNumericClass::Sint, "image table: the entries are wrong");
    require(tables.images[0].words.dwords[0] == guest.heap[4] && tables.images[0].words.dwords[4] == 0u && tables.images[0].dimension == RdnaImageDimension::Dim2D, "image table: an entry holds the wrong words");
    const std::array<Abi::PoisonReason, 5> reasons{Abi::PoisonReason::OutsideSnapshot, Abi::PoisonReason::NotImage, Abi::PoisonReason::Dimension, Abi::PoisonReason::Conversion, Abi::PoisonReason::NotImage};
    for (std::size_t index = 0; index < reasons.size(); index++) require(tables.poison.at(index).reason == reasons[index], "image table: a poison reason is wrong");
    require(tables.poison[4].words.dwords[0] == guest.heap[8u * Stride / 4u + 4u] && tables.poison[4].words.dwords[2] == 0u && tables.poison[4].words.dwords[3] == 0u, "image table: the straddling record's tail is not zero");
    require(capture->specialization.tableViewClasses == std::vector<std::uint8_t>{7u}, "image table: the view classes are wrong");
    auto regions = memory.Regions();
    require(TableCovered(regions, guest.heap.data() + 4, 16u) && TableCovered(regions, guest.heap.data() + 8u * Stride / 4u + 4u, 8u) && !TableTouched(regions, guest.heap.data() + 8u * Stride / 4u + 6u, 8u), "image table: the captured regions are wrong");
    table.request.context.memory = regions;
    bool memoHit = true;
    const auto compiled = Recompile(table.request, *capture, &memoHit);
    require(!memoHit && compiled->bdaAbiVersion == BdaAbi::Version, "image table: the first compile was a memo hit or has no fault ABI");
    bool imageTable = false;
    bool samplerTable = false;
    bool map = false;
    bool fault = false;
    for (const auto& binding : compiled->bindings) {
        if (binding.role == DescriptorRole::ImageTable) {
            imageTable = binding.count == 16u && binding.guestDescriptor.size() == 24u && binding.imageShapes.size() == 3u;
        }
        samplerTable = samplerTable || binding.role == DescriptorRole::SamplerTable;
        map = map || (binding.role == DescriptorRole::ImageTableMap && binding.guestDescriptor == tables.map);
        fault = fault || binding.role == DescriptorRole::FaultBuffer;
    }
    require(imageTable && !samplerTable && map && fault, "image table: the bindings are wrong");
    require(compiled->imageTablePoison.size() == 5u && compiled->imageTablePoison[3].reason == static_cast<std::uint32_t>(Abi::PoisonReason::Conversion), "image table: the result's poison list is wrong");
    const auto repeated = Recompile(table.request, *capture, &memoHit);
    require(memoHit && repeated == compiled, "image table: an equal snapshot was not a memo hit");
#if ANYPS5_ENABLE_SPIRV_TOOLS
    static_cast<void>(ValidateAndOptimizeSpirv(compiled->spirv, table.request.target.vulkanVersion, table.request.target.spirvVersion));
#endif

    guest.heap[4] ^= 1u;
    AgcDriver::ShaderMemory changedMemory({});
    const auto changed = changedMemory.Capture(table.request);
    table.request.context.memory = changedMemory.Regions();
    const auto changedResult = Recompile(table.request, *changed, &memoHit);
    require(!memoHit && changedResult->variantId == compiled->variantId, "image table: a changed T# word was a memo hit or changed the variant");
    guest.heap[4] ^= 1u;

    const auto classesFor = [&](std::uint32_t bytes) {
        fill(guest.heap.data(), bytes);
        AgcDriver::ShaderMemory sized({});
        const auto sizedCapture = sized.Capture(table.request);
        table.request.context.memory = sized.Regions();
        return std::make_pair(sizedCapture->specialization.tableViewClasses, Recompile(table.request, *sizedCapture)->variantId);
    };
    const auto floats = classesFor(2u * Stride + 16u);
    const auto withUint = classesFor(3u * Stride + 16u);
    const auto all = classesFor(HeapBytes);
    const auto fewer = classesFor(7u * Stride + 16u);
    require(floats.first == std::vector<std::uint8_t>{1u} && withUint.first == std::vector<std::uint8_t>{3u} && floats.second != withUint.second, "image table: a Uint entry did not change the classes");
    require(all.first == fewer.first && all.second == fewer.second, "image table: snapshots with equal classes have different variants");

    void* reserved = nullptr;
#ifdef _WIN32
    reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
#else
    reserved = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserved == MAP_FAILED) reserved = nullptr;
#endif
    require(reserved != nullptr, "image table: cannot reserve an inaccessible page");
    fill(reserved, HeapBytes);
    AgcDriver::ShaderMemory unmappedMemory({});
    const auto unmapped = unmappedMemory.Capture(table.request);
    const auto unmappedCodes = ViewCodes(unmapped->snapshot.tables, 0);
    require(unmappedCodes.size() == 9u && std::all_of(unmappedCodes.begin(), unmappedCodes.end(), [](std::uint32_t code) { return code == Abi::PoisonCode(1); }) && unmapped->snapshot.tables.poison[1].reason == Abi::PoisonReason::Unmapped, "image table: an unmapped table is not Unmapped poison");
    require(!TableTouched(unmappedMemory.Regions(), reserved, 4096u), "image table: an unmapped record was read");
#ifdef _WIN32
    VirtualFree(reserved, 0, MEM_RELEASE);
#else
    munmap(reserved, 4096);
#endif

    static std::array<std::uint32_t, 70000u * Stride / 4u> large{};
    fill(large.data(), static_cast<std::uint32_t>(sizeof(large)));
    AgcDriver::ShaderMemory largeMemory({});
    const auto capped = largeMemory.Capture(table.request);
    require(ViewWord(capped->snapshot.tables, 0, Abi::ViewKeyCount) == Abi::MaxKeys && ViewWord(capped->snapshot.tables, 0, Abi::ViewSizeLow) == sizeof(large), "image table: the key cap was not applied");
    fill(guest.heap.data(), HeapBytes, 0x40000facu);
    AgcDriver::ShaderMemory typedMemory({});
    expectFailure([&] { static_cast<void>(typedMemory.Capture(table.request)); }, "not a buffer", "image table: a typed table V# was accepted");
    fill(guest.heap.data(), HeapBytes);

    const std::vector<std::uint32_t> narrowCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u,
        0x7e200500u, 0x8f108410u, 0xf4200406u, 0x20000004u, 0x9310b010u, 0xf4280502u, 0x20000010u, 0xf09c8f08u,
        0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    TableRequest narrow(narrowCode, guest.srt.data());
    const auto narrowPlan = GetResourcePlan(narrow.request);
    require(narrowPlan->info.tableViews.size() == 1u && ColumnOf(*narrowPlan, 0).keyDomain.has_value() && ColumnOf(*narrowPlan, 0).keyDomain->alignment == 16u && ColumnOf(*narrowPlan, 0).keyDomain->offset == 4u, "image table: the material key domain was not recorded");
    auto storedKeysCode = narrowCode;
    storedKeysCode.at(18) = 0x80030000u;
    TableRequest storedKeys(storedKeysCode, guest.srt.data());
    const auto storedKeysPlan = GetResourcePlan(storedKeys.request);
    require(storedKeysPlan->info.tableViews.size() == 1u && !ColumnOf(*storedKeysPlan, 0).keyDomain.has_value(), "image table: a key list the program stores to still narrows the table");
    guest.keys = {0xdeadu, 1u, 0xbeefu, 0xcafeu, 0x5555u, 3u, 0x7777u, 0x9999u};
    AgcDriver::ShaderMemory narrowMemory({});
    const auto narrowed = narrowMemory.Capture(narrow.request);
    const auto narrowCodes = ViewCodes(narrowed->snapshot.tables, 0);
    const auto outside = narrowCodes.at(2);
    require(narrowed->snapshot.tables.poison.at(outside & ~Abi::PoisonFlag).reason == Abi::PoisonReason::OutsideDomain, "image table: a record outside the key domain is not OutsideDomain");
    require(narrowCodes[0] == Abi::ImageCode(Abi::FloatClass, 0) && narrowCodes[1] == Abi::ImageCode(Abi::FloatClass, 0) && (narrowCodes[3] >> Abi::ClassShift) == Abi::SintClass, "image table: a record in the key domain was not resolved");
    for (const auto index : {4u, 5u, 6u, 7u, 8u}) require(narrowCodes.at(index) == outside, "image table: a record outside the key domain was resolved");
    const auto narrowRegions = narrowMemory.Regions();
    require(!TableTouched(narrowRegions, guest.heap.data() + 2u * Stride / 4u, Stride) && TableCovered(narrowRegions, &guest.keys[1], 4u) && TableCovered(narrowRegions, &guest.keys[5], 4u) && !TableTouched(narrowRegions, &guest.keys[2], 12u), "image table: the narrowing read the wrong words");
    const auto pendingQuery = [](std::uint64_t address, std::size_t bytes, std::span<std::byte>) {
        const auto keys = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest.keys.data()));
        return address < keys + sizeof(guest.keys) && keys < address + bytes ? AgcDriver::ShaderMemory::PendingWrite::Sync : AgcDriver::ShaderMemory::PendingWrite::None;
    };
    AgcDriver::ShaderMemory pendingMemory({}, pendingQuery);
    const auto pending = pendingMemory.Capture(narrow.request);
    require(ViewCodes(pending->snapshot.tables, 0) == ViewCodes(tables, 0) && !TableTouched(pendingMemory.Regions(), guest.keys.data(), sizeof(guest.keys)), "image table: a pending key list was scanned");
    static std::array<std::uint32_t, 300u * 4u> manyKeys{};
    const auto manyV = TableV(manyKeys.data(), 0u, static_cast<std::uint32_t>(sizeof(manyKeys)));
    std::copy(manyV.begin(), manyV.end(), guest.srt.begin() + 8);
    AgcDriver::ShaderMemory manyMemory({});
    const auto many = manyMemory.Capture(narrow.request);
    require(ViewCodes(many->snapshot.tables, 0) == ViewCodes(tables, 0) && !TableTouched(manyMemory.Regions(), manyKeys.data(), sizeof(manyKeys)), "image table: a key list over the scan limit was scanned");
    fill(guest.heap.data(), HeapBytes);
}

void verifyImageTableCompare() {
    using namespace ShaderRecompiler;
    namespace Abi = ImageTableAbi;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Format32Float = 22;
    constexpr std::uint32_t Format32UInt = 20;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Stride = 48;
    constexpr std::array<std::uint32_t, 4> CompareS{2u | (2u << 3u) | (2u << 6u) | (3u << 12u), 0u, (1u << 20u) | (1u << 22u), 0u};
    static TableTexture textures[2];
    struct alignas(4096) Guest {
        std::array<std::uint32_t, 3 * Stride / 4> heap{};
        std::array<std::uint32_t, 24> srt{};
        std::array<std::uint32_t, 4> output{};
    };
    static Guest guest;
    const auto heapV = TableV(guest.heap.data(), 0u, static_cast<std::uint32_t>(sizeof(guest.heap)));
    const auto outputV = TableV(guest.output.data(), 0u, 16u);
    std::copy(heapV.begin(), heapV.end(), guest.srt.begin());
    std::copy(CompareS.begin(), CompareS.end(), guest.srt.begin() + 4);
    std::copy(outputV.begin(), outputV.end(), guest.srt.begin() + 12);
    const auto record = [&](std::uint32_t index, std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), guest.heap.begin() + index * (Stride / 4u) + 4u);
    };
    const auto direct = [&](std::uint32_t format) {
        const auto words = TableT(textures[1].bytes.data(), format, Type2D);
        std::fill(guest.srt.begin() + 16, guest.srt.end(), 0u);
        std::copy(words.begin(), words.end(), guest.srt.begin() + 16);
    };
    const auto compile = [&](const std::vector<std::uint32_t>& code, std::uint64_t address) {
        TableRequest table(code, guest.srt.data());
        table.request.shader.codeAddress = address;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(table.request);
        table.request.context.memory = memory.Regions();
        const auto compiled = Recompile(table.request, *capture);
#if ANYPS5_ENABLE_SPIRV_TOOLS
        static_cast<void>(ValidateAndOptimizeSpirv(compiled->spirv, table.request.target.vulkanVersion, table.request.target.spirvVersion));
#endif
        return std::make_pair(capture, compiled);
    };
    const auto rejected = [&](const std::vector<std::uint32_t>& code, std::uint64_t address, const char* expected, const char* message) {
        TableRequest table(code, guest.srt.data());
        table.request.shader.codeAddress = address;
        AgcDriver::ShaderMemory memory({});
        expectFailure([&] { static_cast<void>(memory.Capture(table.request)); }, expected, message);
    };
    const auto binds = [](const RecompileResult& result, DescriptorRole role) {
        return std::any_of(result.bindings.begin(), result.bindings.end(), [&](const DescriptorBinding& binding) { return binding.role == role; });
    };
    const auto comparesDirect = [](const RecompileResult& result) {
        return std::any_of(result.bindings.begin(), result.bindings.end(), [](const DescriptorBinding& binding) {
            return binding.role != DescriptorRole::ImageTable && std::any_of(binding.imageDepthCompare.begin(), binding.imageDepthCompare.end(), [](bool compare) { return compare; });
        });
    };

    const std::vector<std::uint32_t> tableImageCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf0bc8108u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    record(0, TableT(textures[0].bytes.data(), Format32Float, Type2D));
    record(1, {0u, 0u, 0u, 0u});
    record(2, TableT(textures[0].bytes.data(), Format32UInt, Type2D));
    const auto [imageCapture, imageResult] = compile(tableImageCode, 0x51000u);
    const auto& imageTables = imageCapture->snapshot.tables;
    require(imageCapture->plan->info.tableViews.size() == 1u && imageCapture->plan->info.tableViews[0].depthCompare && !imageCapture->plan->info.tableViews[0].sampler, "image table compare: the program has no compare image view");
    require(ViewCodes(imageTables, 0) == std::vector<std::uint32_t>{Abi::ImageCode(Abi::FloatClass, 0), Abi::NullCode, Abi::PoisonCode(1)} && imageTables.poison.at(1).reason == Abi::PoisonReason::NumericClass, "image table compare: the R32 float, null and Uint entries are wrong");
    require(imageTables.images.size() == 1u && imageTables.images[0].depthCompare, "image table compare: the R32 float entry does not compare");
    require(std::any_of(imageResult->bindings.begin(), imageResult->bindings.end(), [](const DescriptorBinding& binding) { return binding.role == DescriptorRole::ImageTable && binding.imageDepthCompare == std::vector<bool>{true}; }), "image table compare: the table entry is not bound for comparison");
    record(1, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    rejected(tableImageCode, 0x51000u, "under comparison sampling, which is emulated only for a T# outside a table", "image table compare: a color table entry under depth compare was accepted");
    record(1, {0u, 0u, 0u, 0u});

    const std::vector<std::uint32_t> tableSamplerCode{0xf4080100u, 0xfa000000u, 0xf40c0200u, 0xfa000040u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf0bc0108u, 0x00a20000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    for (std::uint32_t index = 0; index < 3u; index++) record(index, CompareS);
    direct(Format32Float);
    const auto [samplerCapture, samplerResult] = compile(tableSamplerCode, 0x52000u);
    require(samplerCapture->plan->info.tableViews.size() == 1u && samplerCapture->plan->info.tableViews[0].sampler && samplerCapture->plan->info.tableViews[0].depthCompare, "image table compare: the program has no compare sampler view");
    require(samplerCapture->specialization.images.size() == 1u && samplerCapture->specialization.images[0].emulatedCompare == 0u && comparesDirect(*samplerResult) && binds(*samplerResult, DescriptorRole::SamplerTable), "image table compare: a direct R32 float T# under a table S# left the native comparison");
    direct(Format8888UNorm);
    rejected(tableSamplerCode, 0x52000u, "comparison sampling of a color texture through a sampler table is not implemented", "image table compare: a direct color T# under a table compare S# was accepted");
    std::fill(guest.heap.begin(), guest.heap.end(), 0u);
}

void verifyImageTableSrgb() {
    using namespace ShaderRecompiler;
    namespace Abi = ImageTableAbi;
    constexpr std::uint32_t Format8_8UNorm = 14;
    constexpr std::uint32_t Format8Srgb = 128;
    constexpr std::uint32_t Format8_8Srgb = 129;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Stride = 48;
    static TableTexture textures[3];
    struct alignas(4096) Guest {
        std::array<std::uint32_t, 3 * Stride / 4> heap{};
        std::array<std::uint32_t, 24> srt{};
        std::array<std::uint32_t, 4> output{};
    };
    static Guest guest;
    const auto heapV = TableV(guest.heap.data(), 0u, static_cast<std::uint32_t>(sizeof(guest.heap)));
    const auto outputV = TableV(guest.output.data(), 0u, 16u);
    std::copy(heapV.begin(), heapV.end(), guest.srt.begin());
    std::copy(outputV.begin(), outputV.end(), guest.srt.begin() + 12);
    const auto record = [&](std::uint32_t index, std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), guest.heap.begin() + index * (Stride / 4u) + 4u);
    };
    const auto codes = [&](const std::vector<std::uint32_t>& code, std::uint64_t address, std::uint32_t srgbDecodeFormats) {
        TableRequest table(code, guest.srt.data());
        table.request.shader.codeAddress = address;
        table.request.target.srgbDecodeFormats = srgbDecodeFormats;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(table.request);
        const auto& tables = capture->snapshot.tables;
        std::vector<std::uint32_t> result = ViewCodes(tables, 0);
        for (std::size_t index = 1; index < tables.poison.size(); index++) result.push_back(static_cast<std::uint32_t>(tables.poison[index].reason));
        return result;
    };
    const auto conversion = static_cast<std::uint32_t>(Abi::PoisonReason::Conversion);

    const std::vector<std::uint32_t> tableImageCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    record(0, TableT(textures[0].bytes.data(), Format8Srgb, Type2D));
    record(1, TableT(textures[1].bytes.data(), Format8_8Srgb, Type2D));
    record(2, TableT(textures[2].bytes.data(), Format8_8UNorm, Type2D));
    require(codes(tableImageCode, 0x57000u, 3u) == std::vector<std::uint32_t>{Abi::PoisonCode(1), Abi::PoisonCode(2), Abi::ImageCode(Abi::FloatClass, 0), conversion, conversion}, "image table sRGB: 8_SRGB and 8_8_SRGB entries decoded in the shader are not Conversion poison");
    require(codes(tableImageCode, 0x58000u, 2u) == std::vector<std::uint32_t>{Abi::ImageCode(Abi::FloatClass, 0), Abi::PoisonCode(1), Abi::ImageCode(Abi::FloatClass, 1), conversion}, "image table sRGB: with only 8_8_SRGB decoded in the shader, the entries are wrong");
    require(codes(tableImageCode, 0x59000u, 0u) == std::vector<std::uint32_t>{Abi::ImageCode(Abi::FloatClass, 0), Abi::ImageCode(Abi::FloatClass, 1), Abi::ImageCode(Abi::FloatClass, 2)}, "image table sRGB: sRGB entries the device samples are not bound");

    const std::vector<std::uint32_t> tableSamplerCode{0xf4080100u, 0xfa000000u, 0xf40c0200u, 0xfa000040u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf09c0108u, 0x00a20000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    for (std::uint32_t index = 0; index < 3u; index++) record(index, {0u, 0u, 0u, 0u});
    const auto directT = TableT(textures[1].bytes.data(), Format8_8Srgb, Type2D);
    std::copy(directT.begin(), directT.end(), guest.srt.begin() + 16);
    {
        TableRequest table(tableSamplerCode, guest.srt.data());
        table.request.shader.codeAddress = 0x5a000u;
        table.request.target.srgbDecodeFormats = 2u;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(table.request);
        const auto regions = memory.Regions();
        table.request.context.memory = regions;
        expectFailure([&] { static_cast<void>(Recompile(table.request, *capture)); }, "samples or gathers an sRGB image the device cannot sample", "image table sRGB: a direct 8_8_SRGB T# decoded in the shader was sampled through a table S#");
    }
    std::fill(guest.heap.begin(), guest.heap.end(), 0u);
    std::fill(guest.srt.begin() + 16, guest.srt.end(), 0u);
}

void verifyImageTableReduction() {
    using namespace ShaderRecompiler;
    namespace Abi = ImageTableAbi;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Stride = 48;
    constexpr std::uint32_t MinReduction = 1u << 29u;
    constexpr std::uint32_t Bilinear = (1u << 20u) | (1u << 22u);
    static TableTexture textures[2];
    struct alignas(4096) Guest {
        std::array<std::uint32_t, 2 * Stride / 4> heap{};
        std::array<std::uint32_t, 24> srt{};
        std::array<std::uint32_t, 4> output{};
    };
    static Guest guest;
    const auto heapV = TableV(guest.heap.data(), 0u, static_cast<std::uint32_t>(sizeof(guest.heap)));
    const auto outputV = TableV(guest.output.data(), 0u, 16u);
    const auto directT = TableT(textures[1].bytes.data(), Format8888UNorm, Type2D);
    std::copy(heapV.begin(), heapV.end(), guest.srt.begin());
    std::copy(outputV.begin(), outputV.end(), guest.srt.begin() + 12);
    std::copy(directT.begin(), directT.end(), guest.srt.begin() + 16);
    const auto record = [&](std::uint32_t index, std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), guest.heap.begin() + index * (Stride / 4u) + 4u);
    };
    const auto directS = [&](std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), guest.srt.begin() + 4);
    };
    const auto compile = [&](const std::vector<std::uint32_t>& code, std::uint64_t address) {
        TableRequest table(code, guest.srt.data());
        table.request.shader.codeAddress = address;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(table.request);
        table.request.context.memory = memory.Regions();
        const auto compiled = Recompile(table.request, *capture);
#if ANYPS5_ENABLE_SPIRV_TOOLS
        static_cast<void>(ValidateAndOptimizeSpirv(compiled->spirv, table.request.target.vulkanVersion, table.request.target.spirvVersion));
#endif
        return std::make_pair(capture, compiled);
    };
    const auto samplerWords = [](const RecompileResult& result, DescriptorRole role) {
        const auto binding = std::find_if(result.bindings.begin(), result.bindings.end(), [&](const DescriptorBinding& candidate) { return candidate.role == role; });
        require(binding != result.bindings.end(), "image table reduction: the program has no sampler binding");
        return binding->guestDescriptor;
    };

    const std::vector<std::uint32_t> tableSamplerCode{0xf4080100u, 0xfa000000u, 0xf40c0200u, 0xfa000040u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf09c0108u, 0x00a20000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    record(0, {MinReduction, 0u, 0u, 0u});
    record(1, {0u, 0u, Bilinear, 0u});
    const auto [samplerCapture, samplerResult] = compile(tableSamplerCode, 0x53000u);
    const auto& samplerTables = samplerCapture->snapshot.tables;
    require(ViewCodes(samplerTables, 0) == std::vector<std::uint32_t>{Abi::PoisonCode(1), Abi::SamplerCode(1)} && samplerTables.poison.at(1).reason == Abi::PoisonReason::Reduction, "image table reduction: a min table S# is not Reduction poison");
    const auto tableWords = samplerWords(*samplerResult, DescriptorRole::SamplerTable);
    require(samplerTables.samplers.size() == 2u && tableWords.size() == 16u && std::none_of(tableWords.begin(), tableWords.end(), [](std::uint32_t word) { return word == MinReduction; }), "image table reduction: the min table S# was bound");

    const std::vector<std::uint32_t> tableImageCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
        0xf4280502u, 0x20000010u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    record(0, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    record(1, TableT(textures[1].bytes.data(), Format8888UNorm, Type2D));
    directS({MinReduction, 0u, Bilinear, 0u});
    {
        TableRequest table(tableImageCode, guest.srt.data());
        table.request.shader.codeAddress = 0x54000u;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(table.request);
        const auto regions = memory.Regions();
        table.request.context.memory = regions;
        expectFailure([&] { static_cast<void>(Recompile(table.request, *capture)); }, "a min or max reduction sampler that filters between texels or mip levels samples an image selected at run time", "image table reduction: a bilinear min S# paired with a table T# was accepted");
    }
    directS({MinReduction, 0u, 0u, 0u});
    const auto [pointCapture, pointResult] = compile(tableImageCode, 0x55000u);
    require(ViewCodes(pointCapture->snapshot.tables, 0) == std::vector<std::uint32_t>{Abi::ImageCode(Abi::FloatClass, 0), Abi::ImageCode(Abi::FloatClass, 1)} && samplerWords(*pointResult, DescriptorRole::GuestSamplers) == std::vector<std::uint32_t>{MinReduction, 0u, 0u, 0u}, "image table reduction: a point min S# paired with a table T# was not bound as it is");
    directS({0u, 0u, Bilinear, 0u});
    static_cast<void>(compile(tableImageCode, 0x56000u));
    std::fill(guest.heap.begin(), guest.heap.end(), 0u);
    directS({0u, 0u, 0u, 0u});
}

void verifyPointerImageTable() {
    using namespace ShaderRecompiler;
    namespace Abi = ImageTableAbi;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Format32UInt = 20;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Palette = 0x40;
    constexpr std::uint32_t Entries = 256;
    constexpr std::size_t Committed = 0x1f0000;
    constexpr std::size_t Reserved = 0x400000;
    static TableTexture textures[2];
    void* block = nullptr;
#ifdef _WIN32
    block = VirtualAlloc(nullptr, Reserved, MEM_RESERVE, PAGE_NOACCESS);
    require(block != nullptr && VirtualAlloc(block, Committed, MEM_COMMIT, PAGE_READWRITE) == block, "pointer image table: cannot map the root");
#else
    block = mmap(nullptr, Reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(block != MAP_FAILED && mprotect(block, Committed, PROT_READ | PROT_WRITE) == 0, "pointer image table: cannot map the root");
#endif
    auto* root = static_cast<std::uint32_t*>(block);
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(root));
    static std::array<std::uint32_t, 8> keys{};
    static std::array<std::uint32_t, 4> output{};
    const auto keysV = TableV(keys.data(), 0u, static_cast<std::uint32_t>(sizeof(keys)));
    const auto outputV = TableV(output.data(), 0u, static_cast<std::uint32_t>(sizeof(output)));
    std::copy(keysV.begin(), keysV.end(), root);
    std::copy(outputV.begin(), outputV.end(), root + 4);
    const auto pointer = base + Palette + 0x100u;
    root[12] = static_cast<std::uint32_t>(pointer);
    root[13] = static_cast<std::uint32_t>(pointer >> 32u);
    const auto entry = [&](std::uint32_t index, std::array<std::uint32_t, 4> words) {
        std::copy(words.begin(), words.end(), root + (Palette + index * 32u) / 4u);
    };
    entry(0, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    entry(1, TableT(textures[0].bytes.data(), Format8888UNorm, Type2D));
    entry(2, TableT(textures[1].bytes.data(), Format32UInt, Type2D));
    entry(4, {0x1234u, 0x5678u, 0x9abcu, 0x0000ffacu});
    entry(255, TableT(textures[1].bytes.data(), Format32UInt, Type2D));
    root[(Palette + 5u * 32u) / 4u + 6u] = 7u;

    const std::vector<std::uint32_t> maskedCode{0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0x7e200500u, 0x8f108210u,
        0xf4200444u, 0x20000000u, 0x8711ff11u, 0x000000ffu, 0x8f128511u, 0xf40c0900u, 0x24000040u, 0xf09c0f08u, 0x01090000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    TableRequest masked(maskedCode, root);
    const auto plan = GetResourcePlan(masked.request);
    require(plan->info.tableViews.size() == 1u && !plan->info.tableViews[0].sampler && !plan->info.tableViews[0].r128, "pointer image table: the view is wrong");
    const auto& column = ColumnOf(*plan, 0);
    require(column.address && column.stride == 32u && column.addend == 0u && column.offset == Palette && column.dwordCount == 8u && column.maxKey == 0xffu && !column.keyDomain.has_value(), "pointer image table: the column is wrong");
    require(plan->descriptorSources.at(column.heapSource).dwordCount == 2u, "pointer image table: the table base is not a 64-bit pointer");
    std::uint32_t planningOnly = 0;
    for (const auto& memory : plan->memoryInfo) {
        if (memory.kind == ResourceKind::ScalarAddress && memory.offset >= Palette && memory.offset < Palette + 32u && memory.planningOnly) planningOnly++;
    }
    require(planningOnly == 8u && !plan->info.usesDma && plan->info.usesFaultBuffer, "pointer image table: the descriptor loads still read through BDA");

    std::vector<std::uint32_t> expected(Entries, Abi::NullCode);
    expected[0] = expected[1] = Abi::ImageCode(Abi::FloatClass, 0);
    expected[2] = expected[255] = Abi::ImageCode(Abi::UintClass, 1);
    expected[4] = Abi::PoisonCode(1);
    expected[5] = Abi::PoisonCode(2);
    AgcDriver::ShaderMemory memory({});
    const auto capture = memory.Capture(masked.request);
    const auto& tables = capture->snapshot.tables;
    require(ViewWord(tables, 0, Abi::ViewKeyCount) == Entries && ViewWord(tables, 0, Abi::ViewSizeLow) == 0u && ViewWord(tables, 0, Abi::ViewSizeHigh) == 0u && ViewWord(tables, 0, Abi::ViewBaseLow) == static_cast<std::uint32_t>(base) && ViewWord(tables, 0, Abi::ViewBaseHigh) == static_cast<std::uint32_t>(base >> 32u), "pointer image table: the map header is wrong");
    require(ViewCodes(tables, 0) == expected, "pointer image table: the map codes are wrong");
    require(tables.images.size() == 2u && tables.images[1].words.dwordCount == 8u && tables.images[1].words.dwords[0] == root[(Palette + 2u * 32u) / 4u], "pointer image table: the entries are wrong");
    require(tables.poison.size() == 3u && tables.poison[1].reason == Abi::PoisonReason::NotImage && tables.poison[1].address == base + Palette + 4u * 32u && tables.poison[2].words.dwords[6] == 7u, "pointer image table: the poison entries are wrong");
    require(tables.ranges == std::vector<ImageTableRange>{{base + Palette, Entries * 32u}}, "pointer image table: the snapshot range is wrong");
    require(TableCovered(memory.Regions(), root + Palette / 4u, Entries * 32u) && !TableTouched(memory.Regions(), root + (Palette + Entries * 32u) / 4u, Committed - Palette - Entries * 32u), "pointer image table: the captured regions are wrong");
    masked.request.context.memory = memory.Regions();
    const auto compiled = Recompile(masked.request, *capture);
    const bool map = std::any_of(compiled->bindings.begin(), compiled->bindings.end(), [&](const DescriptorBinding& binding) { return binding.role == DescriptorRole::ImageTableMap && binding.guestDescriptor == tables.map; });
    require(map && compiled->imageTableRanges.size() == 1u && compiled->imageTableRanges[0].first == base + Palette, "pointer image table: the compiled table is wrong");
#if ANYPS5_ENABLE_SPIRV_TOOLS
    static_cast<void>(ValidateAndOptimizeSpirv(compiled->spirv, masked.request.target.vulkanVersion, masked.request.target.spirvVersion));
#endif

    const std::vector<std::uint32_t> pointerCode{0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0xf4040500u, 0xfa000030u,
        0x7e200500u, 0x8f108210u, 0xf4200444u, 0x20000000u, 0x8711ff11u, 0x000000ffu, 0x8f128511u, 0xf40c090au, 0x241fff00u, 0xf09c0f08u, 0x01090000u,
        0xe0700000u, 0x80070000u, 0xbf810000u};
    TableRequest loaded(pointerCode, root);
    require(ColumnOf(*GetResourcePlan(loaded.request), 0).offset == 0xffffff00u, "pointer image table: a negative immediate was not kept");
    AgcDriver::ShaderMemory loadedMemory({});
    const auto loadedCapture = loadedMemory.Capture(loaded.request);
    require(ViewCodes(loadedCapture->snapshot.tables, 0) == expected && loadedCapture->snapshot.tables.ranges == tables.ranges, "pointer image table: a base loaded from the SRT with a negative immediate snapshots other records");
    root[12] = static_cast<std::uint32_t>(pointer) | 2u;
    AgcDriver::ShaderMemory unalignedMemory({});
    const auto unaligned = unalignedMemory.Capture(loaded.request);
    const auto unalignedCodes = ViewCodes(unaligned->snapshot.tables, 0);
    require(unalignedCodes.size() == Entries && std::all_of(unalignedCodes.begin(), unalignedCodes.end(), [&](std::uint32_t code) { return code == unalignedCodes[0]; }) && unaligned->snapshot.tables.poison.at(unalignedCodes[0] & ~Abi::PoisonFlag).reason == Abi::PoisonReason::Unmapped, "pointer image table: a pointer that is not dword-aligned was read");
    root[12] = static_cast<std::uint32_t>(pointer);

    const std::vector<std::uint32_t> directCode{0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0xf4000440u, 0xfa000038u, 0x8f128511u, 0xf40c0900u,
        0x24000040u, 0xf09c0f08u, 0x01090000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    root[14] = 2u;
    TableRequest direct(directCode, root);
    const auto directPlan = GetResourcePlan(direct.request);
    require(directPlan->info.tableViews.empty() && directPlan->info.images.size() == 1u && directPlan->info.images[0].tableView == NoTableView, "pointer image table: a T# at an offset known at record time is not a direct binding");
    AgcDriver::ShaderMemory directMemory({});
    const auto directCapture = directMemory.Capture(direct.request);
    require(directCapture->snapshot.images.size() == 1u && directCapture->snapshot.images[0].dwords[0] == root[(Palette + 2u * 32u) / 4u] && !TableTouched(directMemory.Regions(), root + (Palette + 3u * 32u) / 4u, Committed - Palette - 3u * 32u), "pointer image table: a T# at an offset known at record time read other records");
    root[14] = 0u;

    const std::vector<std::uint32_t> wideCode{0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0x7e220500u, 0x8f128511u, 0xf40c0900u, 0x24000040u,
        0xf09c0f08u, 0x01090000u, 0xe0700000u, 0x80070000u, 0xbf810000u};
    TableRequest wide(wideCode, root);
    const auto widePlan = GetResourcePlan(wide.request);
    require(ColumnOf(*widePlan, 0).maxKey == 0xffffffffu && !ColumnOf(*widePlan, 0).keyDomain.has_value(), "pointer image table: a lane key has a bound or a key domain");
    AgcDriver::ShaderMemory wideMemory({});
    const auto wideCapture = wideMemory.Capture(wide.request);
    const auto wideCodes = ViewCodes(wideCapture->snapshot.tables, 0);
    constexpr std::uint32_t mapped = (Committed - Palette) / 32u;
    require(wideCodes.size() == Abi::MaxKeys && std::equal(expected.begin(), expected.end(), wideCodes.begin()) && wideCodes[mapped - 1u] == Abi::NullCode, "pointer image table: an unbounded key did not snapshot the capped records");
    const auto unmapped = wideCodes[mapped];
    require((unmapped & Abi::PoisonFlag) != 0u && wideCapture->snapshot.tables.poison.at(unmapped & ~Abi::PoisonFlag).reason == Abi::PoisonReason::Unmapped && wideCodes.back() == unmapped, "pointer image table: records past the mapping are not Unmapped poison");
    require(!TableTouched(wideMemory.Regions(), root + Committed / 4u, Reserved - Committed), "pointer image table: an unmapped record was read");

    auto narrowCode = maskedCode;
    narrowCode.erase(narrowCode.begin() + 10, narrowCode.begin() + 12);
    keys = {2u, 255u, 0x102u, 2u, 0u, 0u, 0u, 0u};
    TableRequest narrow(narrowCode, root);
    const auto narrowPlan = GetResourcePlan(narrow.request);
    require(ColumnOf(*narrowPlan, 0).keyDomain.has_value() && ColumnOf(*narrowPlan, 0).maxKey == 0xffffffffu, "pointer image table: a key read from a V# has no key domain");
    AgcDriver::ShaderMemory narrowMemory({});
    const auto narrowCapture = narrowMemory.Capture(narrow.request);
    const auto narrowCodes = ViewCodes(narrowCapture->snapshot.tables, 0);
    const auto outsideDomain = narrowCodes.at(1);
    require(narrowCodes.size() == Abi::MaxKeys && narrowCodes[0] == expected[0] && narrowCodes[2] == expected[2] && narrowCodes[255] == expected[255] && narrowCodes[0x102] == Abi::NullCode, "pointer image table: a record in the key domain was not resolved");
    require((outsideDomain & Abi::PoisonFlag) != 0u && narrowCapture->snapshot.tables.poison.at(outsideDomain & ~Abi::PoisonFlag).reason == Abi::PoisonReason::OutsideDomain && narrowCodes[4] == outsideDomain && narrowCodes.back() == outsideDomain, "pointer image table: a record outside the key domain is not OutsideDomain poison");
    require(!TableTouched(narrowMemory.Regions(), root + (Palette + 32u) / 4u, 32u) && !TableTouched(narrowMemory.Regions(), root + Committed / 4u, Reserved - Committed), "pointer image table: a record outside the key domain was read");
    keys = {};
#ifdef _WIN32
    VirtualFree(block, 0, MEM_RELEASE);
#else
    munmap(block, Reserved);
#endif
}

void verifyDescriptorPhis() {
    using namespace ShaderRecompiler;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Type2D = 9;
    struct alignas(256) Texture { std::array<std::uint8_t, 256> bytes{}; };
    static Texture textures[2];
    const auto imageDescriptor = [&](const Texture& texture) {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture.bytes.data()));
        return std::array<std::uint32_t, 8>{static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (Type2D << 28u), 0u, 0u, 0u, 0u};
    };
    const std::array<std::uint32_t, 4> pointWrap{0u, 0u, 0u, 0u};
    const std::array<std::uint32_t, 4> linearMirror{0x49u, 0u, 0x00500000u, 0u};
    std::array<std::uint32_t, 4> output{};
    const auto outputAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output.data()));
    std::array<std::uint32_t, 32> srt{};
    const auto first = imageDescriptor(textures[0]);
    const auto second = imageDescriptor(textures[1]);
    std::copy(first.begin(), first.end(), srt.begin());
    std::copy(pointWrap.begin(), pointWrap.end(), srt.begin() + 8);
    std::copy(linearMirror.begin(), linearMirror.end(), srt.begin() + 12);
    const std::array<std::uint32_t, 4> outputV{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu), 16u, 0xfacu};
    std::copy(outputV.begin(), outputV.end(), srt.begin() + 16);
    srt[20] = 1u;
    std::copy(second.begin(), second.end(), srt.begin() + 24);
    const auto srtAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(srt.data()));
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    const std::array<std::uint32_t, 1> capabilities{29u};
    const auto makeRequest = [&](const std::vector<std::uint32_t>& code, std::uint32_t waveSize = 32u) {
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x21000u, code, 0, {}};
        request.context.waveSize = waveSize;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{waveSize, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = capabilities;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;
        return request;
    };
    const auto countOps = [](const std::vector<std::uint32_t>& words, std::uint32_t opcode) {
        std::size_t count = 0;
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto length = words[cursor] >> 16u;
            require(length != 0 && length <= words.size() - cursor, "descriptor Phi: truncated SPIR-V instruction");
            count += (words[cursor] & 0xffffu) == opcode ? 1u : 0u;
            cursor += length;
        }
        return count;
    };
    constexpr std::uint32_t OpImageSampleExplicitLod = 88;

    const std::vector<std::uint32_t> samplerCode{0xf40c0200u, 0xfa000000u, 0xf4000400u, 0xfa000050u, 0xbf8cc07fu, 0xbf068010u, 0xbf850003u, 0xf4080500u, 0xfa000020u, 0xbf820002u, 0xf4080500u, 0xfa000030u, 0xf4080600u, 0xfa000040u, 0xbf8cc07fu, 0xf09c0f08u, 0x00a20000u, 0xbf8c3f70u, 0xe0700000u, 0x80060000u, 0xbf810000u};
    const std::vector<std::uint32_t> imageCode{0xf4000400u, 0xfa000050u, 0xf4080500u, 0xfa000020u, 0xbf8cc07fu, 0xbf068010u, 0xbf850003u, 0xf40c0200u, 0xfa000000u, 0xbf820002u, 0xf40c0200u, 0xfa000060u, 0xf4080600u, 0xfa000040u, 0xbf8cc07fu, 0xf09c0f08u, 0x00a20000u, 0xbf8c3f70u, 0xe0700000u, 0x80060000u, 0xbf810000u};
    const std::vector<std::uint32_t> dynamicCode{0xf40c0200u, 0xfa000000u, 0xf4000400u, 0xfa000050u, 0xbf8cc07fu, 0xbf068010u, 0xbf850003u, 0xf4080500u, 0xfa000020u, 0xbf820002u, 0xf4080500u, 0x20000000u, 0xf4080600u, 0xfa000040u, 0xbf8cc07fu, 0xf09c0f08u, 0x00a20000u, 0xbf8c3f70u, 0xe0700000u, 0x80060000u, 0xbf810000u};

    const auto compile = [&](const std::vector<std::uint32_t>& code, std::size_t images, std::size_t samplers) {
        auto request = makeRequest(code);
        const auto plan = GetResourcePlan(request);
        require(plan->info.images.size() == images && plan->info.samplers.size() == samplers && plan->info.sampledPairs.size() == 2u, "descriptor Phi: the edges were not given one resource each");
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(request);
        request.context.memory = memory.Regions();
        const auto compiled = Recompile(request, *capture);
        require(countOps(compiled->spirv, OpImageSampleExplicitLod) == 2u, "descriptor Phi: the SPIR-V does not sample once per edge");
#if ANYPS5_ENABLE_SPIRV_TOOLS
        static_cast<void>(ValidateAndOptimizeSpirv(compiled->spirv, request.target.vulkanVersion, request.target.spirvVersion));
#endif
        return capture;
    };
    const auto samplerCapture = compile(samplerCode, 1u, 2u);
    const auto& samplers = samplerCapture->snapshot.samplers;
    require(samplers.size() == 2u, "descriptor Phi: the snapshot does not hold both S#s");
    const auto holds = [&](const std::array<std::uint32_t, 4>& words) {
        return std::ranges::any_of(samplers, [&](const DescriptorValue& value) {
            return value.dwordCount == 4u && std::equal(words.begin(), words.end(), value.dwords.begin());
        });
    };
    require(holds(pointWrap) && holds(linearMirror), "descriptor Phi: the snapshot S#s are not the two edges' S#s");

    const auto imageCapture = compile(imageCode, 2u, 1u);
    const auto& images = imageCapture->snapshot.images;
    require(images.size() == 2u, "descriptor Phi: the snapshot does not hold both T#s");
    const auto holdsImage = [&](const std::array<std::uint32_t, 8>& words) {
        return std::ranges::any_of(images, [&](const DescriptorValue& value) {
            return std::equal(words.begin(), words.end(), value.dwords.begin());
        });
    };
    require(holdsImage(first) && holdsImage(second), "descriptor Phi: the snapshot T#s are not the two edges' T#s");

    auto twoLane = makeRequest(samplerCode, 64u);
    AgcDriver::ShaderMemory twoLaneMemory({});
    const auto twoLaneCapture = twoLaneMemory.Capture(twoLane);
    twoLane.context.memory = twoLaneMemory.Regions();
    require(countOps(Recompile(twoLane, *twoLaneCapture)->spirv, OpImageSampleExplicitLod) == 4u, "descriptor Phi: the two-lane SPIR-V does not sample once per edge and half");

    auto dynamic = makeRequest(dynamicCode);
    expectFailure([&] { static_cast<void>(GetResourcePlan(dynamic)); }, "GetSamplerResource dword 0 is not a valid runtime value", "descriptor Phi: an edge without an SRT slot was accepted");
}

void verifyProgramCounterRelativeData() {
    using namespace ShaderRecompiler;
    static const std::array<std::uint32_t, 15> code{
        0xbe801f00u,
        0x800000ffu, 52u,
        0x82010180u,
        0xb0020010u,
        0xbe8303ffu, 0x10005004u,
        0xf4200100u, 0xfa000000u,
        0xbf8cc07fu,
        0x7e000204u,
        0xf80008cfu, 0u,
        0xbf810000u,
        0x3f800000u
    };
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
    RecompileRequest request{};
    request.shader = {ShaderStage::Vertex, codeAddress, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 8;
    request.context.vertex = ShaderVertexStageInfo{};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    const auto dataBase = [](const RecompileResult& result) {
        for (const auto& binding : result.bindings) {
            if (binding.role != DescriptorRole::GuestBuffers || binding.guestDescriptor.size() < 4u) continue;
            return static_cast<std::uint64_t>(binding.guestDescriptor[0]) | (static_cast<std::uint64_t>(binding.guestDescriptor[1] & 0xffffu) << 32u);
        }
        throw std::runtime_error("program counter data: no guest buffer was bound");
    };
    AgcDriver::ShaderMemory memory({});
    static_cast<void>(memory.Capture(request));
    request.context.memory = memory.Regions();
    const auto first = Recompile(request);
    require(dataBase(first) == codeAddress + 56u, "program counter data: the V# does not name the data at the shader's address");
    auto relocated = request;
    relocated.shader.codeAddress += 0x1000u;
    const auto moved = Recompile(relocated);
    require(moved.cacheHit, "program counter data: relocating the shader recompiled it");
    require(dataBase(moved) == codeAddress + 0x1000u + 56u, "program counter data: the relocated shader bound the old address");
}

void verifyLanesOutsideHostSubgroup() {
    using namespace ShaderRecompiler;
    const auto pixel = [](std::span<const std::uint32_t> code, std::uint32_t subgroupSize) {
        ShaderPixelStageInfo info{};
        info.inputAddr = PixelInputBit(PixelInput::PerspectiveCenter);
        info.hasPerspectiveCenterVgpr = true;
        info.targetOutputMode[0] = 9u;
        info.targetExportMapping.fill(0xe4u);
        RecompileRequest request{};
        request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.pixel = info;
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = subgroupSize;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        return !Recompile(request).spirv.empty();
    };
    const auto compute = [](std::span<const std::uint32_t> code, std::uint32_t subgroupSize) {
        static constexpr std::array<std::uint32_t, 4> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = subgroupSize;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        return !Recompile(request).spirv.empty();
    };
    static constexpr std::array<std::uint32_t, 6> lane63{0xd7600006u, 0x00017f00u, 0x7e020206u, 0xf800180fu, 0x01010101u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 6> lane31{0xd7600006u, 0x00013f00u, 0x7e020206u, 0xf800180fu, 0x01010101u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 11> waterfall{0xbe80047eu, 0xbe821400u, 0xd7600003u, 0x00000500u, 0xbe801c02u, 0xbf138000u, 0xbf85fffau, 0x7e020203u, 0xf800180fu, 0x01010101u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 6> computeLane63{0xd7600006u, 0x00017f00u, 0x7e020206u, 0xe0700000u, 0x80000100u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 5> permlanex16{0xd7780001u, 0x02010100u, 0xf800180fu, 0x01010101u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 5> permlane16{0xd7770001u, 0x02010100u, 0xf800180fu, 0x01010101u, 0xbf810000u};
    expectFailure([&] { static_cast<void>(pixel(lane63, 32u)); }, "v_readlane_b32 of lane 63 is outside the 32-lane host subgroup that runs this wave64 program at one lane per invocation", "host lanes: a wave64 pixel program read lane 63 of a 32-lane subgroup");
    expectFailure([&] { static_cast<void>(pixel(lane63, 8u)); }, "v_readlane_b32 of lane 63 is outside the 8-lane host subgroup", "host lanes: a wave64 pixel program read lane 63 of an 8-lane subgroup");
    require(pixel(lane31, 32u), "host lanes: a wave64 pixel program did not build reading lane 31 of a 32-lane subgroup");
    expectFailure([&] { static_cast<void>(pixel(lane31, 8u)); }, "v_readlane_b32 of lane 31 is outside the 8-lane host subgroup", "host lanes: a wave64 pixel program read lane 31 of an 8-lane subgroup");
    require(pixel(lane63, 64u), "host lanes: a wave64 pixel program did not build reading lane 63 of a 64-lane subgroup");
    require(compute(computeLane63, 32u), "host lanes: a wave64 compute program did not build reading lane 63 at two lanes per invocation");
    require(compute(computeLane63, 8u), "host lanes: a wave64 compute program did not build reading lane 63 of an 8-lane subgroup");
    require(pixel(waterfall, 32u) && pixel(waterfall, 8u), "host lanes: an s_ff1_i32_b64 waterfall reading a lane in a register did not build");
    expectFailure([&] { static_cast<void>(pixel(permlanex16, 8u)); }, "v_permlanex16_b32 from lanes 16-31 is outside the 8-lane host subgroup", "host lanes: v_permlanex16_b32 read lanes 16-31 of an 8-lane subgroup");
    require(pixel(permlanex16, 32u) && pixel(permlane16, 8u), "host lanes: a v_permlane16_b32 row that the host subgroup may hold did not build");
}

void verifyMeshConfiguration() {
    using namespace ShaderRecompiler;
    ShaderMeshInputInfo list;
    list.inputPrimitive = 4u;
    require(list.InputPrimitiveSize() == 3u && list.InputPrimitiveStep() == 3u && list.InputVertexCount(21u) == 63u && list.InputPrimitiveCount(63u) == 21u && list.InputPrimitiveCount(2u) == 0u && list.InputVertexCount(0u) == 0u, "triangle list subgroup sizes changed");
    ShaderMeshInputInfo strip;
    strip.inputPrimitive = 6u;
    require(strip.InputPrimitiveSize() == 3u && strip.InputPrimitiveStep() == 1u && strip.InputVertexCount(21u) == 23u && strip.InputPrimitiveCount(23u) == 21u, "triangle strip subgroup sizes changed");
    ShaderMeshInputInfo fan;
    fan.inputPrimitive = 5u;
    require(fan.InputPrimitiveSize() == 3u && fan.InputPrimitiveStep() == 1u && fan.InputVertexCount(30u) == 32u && fan.InputPrimitiveCount(32u) == 30u && fan.InputPrimitiveCount(2u) == 0u, "triangle fan subgroup sizes changed");
    ShaderMeshInputInfo lines;
    lines.inputPrimitive = 2u;
    ShaderMeshInputInfo points;
    points.inputPrimitive = 1u;
    require(lines.InputPrimitiveSize() == 2u && lines.InputPrimitiveStep() == 2u && points.InputPrimitiveSize() == 1u && points.InputVertexCount(5u) == 5u, "line or point subgroup sizes changed");

    static constexpr std::array<std::uint32_t, 1> code{0xbf810000u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Mesh, 0x10000u, code, 0, {}};
    request.context.waveSize = 64;
    const MeshConfiguration mesh{4u, 21u, 63u, 64u, 21u, 64u, 256u, 0u, 12u};
    request.graphics = GraphicsCompileContext{0u, {}, mesh, std::nullopt, {}};
    const auto replay = RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(request));
    require(replay.request.graphics.has_value() && replay.request.graphics->mesh.has_value() && replay.request.graphics->mesh->esgsItemSize == 12u && replay.request.graphics->mesh->primitivesPerGroup == 21u, "mesh configuration was lost in serialization");
    std::vector<std::uint64_t> key;
    RecompileCacheKey::Build(request, key);
    const auto first = key;
    auto other = request;
    auto otherMesh = mesh;
    otherMesh.esgsItemSize = 16u;
    other.graphics = GraphicsCompileContext{0u, {}, otherMesh, std::nullopt, {}};
    RecompileCacheKey::Build(other, key);
    require(key != first && RecompileCacheKey::ContextHash(request) != RecompileCacheKey::ContextHash(other), "the cache keys ignore the mesh configuration");
}

ShaderRecompiler::ShaderPixelStageInfo twoParameterPixel() {
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 2u;
    pixel.interpolatorSettings[1] = 1u;
    pixel.wave32 = true;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PerspectiveCenter) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::LinearCenter);
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.noPerspective = true;
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping[0] = 0xe4u;
    return pixel;
}

std::string requestPrefix(std::string_view text, std::size_t bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result(text.substr(0, ((bytes + 2u) / 3u) * 4u));
    if (bytes % 3u == 1u) {
        result[result.size() - 3u] = alphabet[alphabet.find(result[result.size() - 3u]) & 0x30u];
        result[result.size() - 2u] = '=';
        result.back() = '=';
    } else if (bytes % 3u == 2u) {
        result[result.size() - 2u] = alphabet[alphabet.find(result[result.size() - 2u]) & 0x3cu];
        result.back() = '=';
    }
    return result;
}

void verifyPixelRequestSerialization() {
    using namespace ShaderRecompiler;
    const auto fields = [](const ShaderPixelStageInfo& value) {
        return std::tie(value.interpolatorCount, value.interpolatorSettings, value.wave32, value.inputAddr,
                        value.hasPerspectiveCenterVgpr, value.perspectiveCentroid, value.posX, value.posY,
                        value.posZ, value.posW, value.frontFace, value.ancillary, value.sampleShading,
                        value.noPerspective, value.linearCentroid, value.pixelKillEnable, value.depthExportEnable,
                        value.sampleMaskExportEnable, value.earlyZ, value.executeOnNoop, value.conservativeZExport,
                        value.targetOutputMode, value.targetExportMapping);
    };
    const std::array<std::uint32_t, 1> code{0xbf810000u};
    const std::array<std::uint32_t, 3> userData{0x12345678u, 0xabcdef01u, 0x87654321u};
    const std::array<MemoryRegion, 1> memory{{{0x60000u, std::as_bytes(std::span(userData))}}};
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userData = userData;
    request.context.memory = memory;
    request.context.vertex = ShaderVertexStageInfo{};
    request.context.vertex->fetchAttribReg = 17u;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.nonConstantImageOffsets = true;
    request.layout = {0u, 11u, 16u, 128u};
    request.useCache = false;
    ShaderPixelStageInfo pixel{
        .interpolatorCount = 32u,
        .wave32 = true,
        .inputAddr = 0x7fffu,
        .hasPerspectiveCenterVgpr = true,
        .perspectiveCentroid = true,
        .posX = true,
        .posY = true,
        .posZ = true,
        .posW = true,
        .frontFace = true,
        .ancillary = true,
        .sampleShading = true,
        .noPerspective = true,
        .linearCentroid = true,
        .pixelKillEnable = true,
        .depthExportEnable = true,
        .sampleMaskExportEnable = true,
        .earlyZ = true,
        .executeOnNoop = true,
        .conservativeZExport = ConservativeZExport::GreaterThanZ,
        .targetOutputMode = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}
    };
    for (std::uint32_t i = 0; i < pixel.interpolatorSettings.size(); ++i) pixel.interpolatorSettings[i] = 0x10101010u + i;
    const std::array<std::array<std::uint8_t, 8>, 3> mappings{{
        {0x00u, 0xe4u, 0xc6u, 0x1bu, 0xffu, 0x80u, 0x55u, 0xaau},
        {0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u},
        {0xc6u, 0x1bu, 0x00u, 0xffu, 0x55u, 0xaau, 0x80u, 0x39u}
    }};
    const RequestSerializer serializer;
    for (const auto& mapping : mappings) {
        pixel.targetExportMapping = mapping;
        request.context.pixel = pixel;
        const auto replay = serializer.Deserialize(serializer.Serialize(request));
        require(replay.request.context.pixel.has_value() && fields(*replay.request.context.pixel) == fields(pixel), "pixel fields or export mappings were lost in serialization");
        require(replay.request.context.vertex.has_value() && replay.request.context.vertex->fetchAttribReg == 17u && replay.request.context.memory.size() == 1u && replay.request.context.memory[0].guestAddress == 0x60000u && replay.request.context.memory[0].bytes.size() == sizeof(userData), "pixel mappings displaced the following guest context");
        require(replay.request.target.subgroupSize == 64u && replay.request.target.nonConstantImageOffsets && replay.request.layout.firstBinding == 11u && replay.request.layout.pushConstantOffsetBytes == 16u && !replay.request.useCache, "pixel mappings displaced the following request fields");
        std::vector<std::uint64_t> key;
        RecompileCacheKey::Build(request, key);
        std::vector<std::uint64_t> replayKey;
        RecompileCacheKey::Build(replay.request, replayKey);
        require(key == replayKey && RecompileCacheKey::ContextHash(request) == RecompileCacheKey::ContextHash(replay.request), "pixel replay changed shader identity");
        for (std::size_t i = 0; i < mapping.size(); ++i) {
            auto changed = request;
            changed.context.pixel->targetExportMapping[i] ^= 1u;
            RecompileCacheKey::Build(changed, replayKey);
            require(key != replayKey && RecompileCacheKey::ContextHash(request) != RecompileCacheKey::ContextHash(changed), "shader identity ignored a pixel export mapping");
        }
    }
    request.context.pixel.reset();
    request.shader.stage = ShaderStage::Vertex;
    const auto withoutPixel = serializer.Deserialize(serializer.Serialize(request));
    require(!withoutPixel.request.context.pixel.has_value() && withoutPixel.request.context.vertex.has_value() && withoutPixel.request.context.vertex->fetchAttribReg == 17u && withoutPixel.request.context.memory.size() == 1u && withoutPixel.request.context.memory[0].guestAddress == 0x60000u && withoutPixel.request.target.nonConstantImageOffsets && withoutPixel.request.layout.firstBinding == 11u && !withoutPixel.request.useCache, "a request without pixel state was misaligned");

    RecompileRequest minimal{};
    minimal.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    minimal.context.waveSize = 64;
    minimal.context.pixel = ShaderPixelStageInfo{};
    const auto encoded = serializer.Serialize(minimal);
    require(requestPrefix(encoded, 8u) == "NVNQQQkAAAA=", "new requests did not use serialization version 9");
    constexpr std::size_t mappingOffset = 8u + 37u + 18u + 162u;
    for (std::size_t bytes = 0; bytes < 8u; ++bytes) {
        expectFailure([&] { static_cast<void>(serializer.Deserialize(requestPrefix(encoded, mappingOffset + bytes))); }, "truncated data", "a truncated version-8 pixel mapping was accepted");
    }
    for (const auto unsupported : {"NVNQQQAAAAA=", "NVNQQQoAAAA="}) {
        expectFailure([&] { static_cast<void>(serializer.Deserialize(unsupported)); }, "serialization version", "an unsupported request version was accepted");
    }
}

void verifyLegacyPixelRequests() {
    using namespace ShaderRecompiler;
    static constexpr std::array<std::string_view, 8> legacyPixelRequests{
        "NVNQQQEAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAkAAAAAAAAAAAAA"
        "AAAAAAAAABBAAAADAQBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALAAAAAAAAAIAA"
        "AAAA",
        "NVNQQQIAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAkAAAAAAAAAAAAA"
        "AAAAAAAAABBAAAADAQBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALAAAAAAAAAIAA"
        "AAAAAA==",
        "NVNQQQMAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAkAAAAAAAAAAAAA"
        "AAAAAAAAABBAAAADAQBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALAAAAAAAAAIAA"
        "AAAAAA==",
        "NVNQQQQAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAkAAAAAAAAAAAAA"
        "AAAAAAAAABBAAAADAQBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALAAAAAAAAAIAA"
        "AAAAAA==",
        "NVNQQQUAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAABAAAAAAAAAAAAAAAAAAAACQAAAAAAAAAA"
        "AAAAAAAAAAAAEEAAAAMBAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAsAAAAAAAAA"
        "gAAAAAAA",
        "NVNQQQYAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAABAAAAAAAAAAAAAAAAAAAACQAAAAAAAAAA"
        "AAAAAAAAAAAAEEAAAAMBAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAsAAAAAAAAA"
        "gAAAAAAAAQ==",
        "NVNQQQcAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAABAAAAAAAAAAAAAAAAAAAAAgkAAAAAAAAA"
        "AAAAAAAAAAAAABBAAAADAQBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALAAAAAAAA"
        "AIAAAAAAAAE=",
        "NVNQQQgAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAABAAAAAAAAAAAAAAAAAAAAAgkAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAEEAAAAMBAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAsAAAAAAAAAgAAAAAAAAQ==",
    };
    const RequestSerializer serializer;
    for (std::size_t index = 0; index < legacyPixelRequests.size(); ++index) {
        const auto version = index + 1u;
        const auto replay = serializer.Deserialize(legacyPixelRequests[index]);
        const auto& request = replay.request;
        require(request.context.pixel.has_value(), "legacy pixel state was lost");
        const auto& pixel = *request.context.pixel;
        require(pixel.targetExportMapping == std::array<std::uint8_t, 8>{}, "legacy pixel mapping no longer defaults to zero");
        require(pixel.inputAddr == 2u && pixel.hasPerspectiveCenterVgpr && pixel.targetOutputMode[0] == 9u, "legacy pixel layout was misread");
        require(pixel.conservativeZExport == (version >= 7u ? ConservativeZExport::GreaterThanZ : ConservativeZExport::AnyZ), "legacy conservative Z layout was misread");
        require(request.shader.stage == ShaderStage::Fragment && request.shader.code.size() == 1u && request.shader.code[0] == 0xbf810000u && !request.context.vertex.has_value() && request.context.memory.empty(), "legacy guest context was misaligned");
        require(request.target.vulkanVersion == 0x00401000u && request.target.spirvVersion == 0x00010300u && request.target.subgroupSize == 64u && request.layout.firstBinding == 11u && request.layout.pushConstantSizeBytes == 128u, "legacy target or binding layout was misaligned");
        require(request.useCache == (version == 1u) && request.target.nonConstantImageOffsets == (version >= 6u) && request.target.srgbDecodeFormats == 0u, "legacy request trailer was misread");
        const auto upgraded = serializer.Deserialize(serializer.Serialize(request));
        require(upgraded.request.context.pixel->targetExportMapping == pixel.targetExportMapping && RecompileCacheKey::ContextHash(upgraded.request) == RecompileCacheKey::ContextHash(request), "upgrading a legacy capture changed its pixel mapping");
    }
}

void verifyPixelExportReplay() {
    using namespace ShaderRecompiler;
    std::array<std::uint32_t, 11> code{
        0x7e0002ffu, 0x3e800000u, 0x7e0202ffu, 0x3f000000u,
        0x7e0402ffu, 0x3f400000u, 0x7e0602ffu, 0x3f800000u,
        0xf800180fu, 0x03020100u, 0xbf810000u
    };
    const RequestSerializer serializer;
    for (const auto target : {0u, 7u}) {
        code[8] = 0xf800180fu | (target << 4u);
        ShaderPixelStageInfo pixel{};
        pixel.targetOutputMode[target] = 9u;
        pixel.targetExportMapping.fill(0xe4u);
        pixel.targetExportMapping[target] = 0xc6u;
        RecompileRequest request{};
        request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
        request.context.waveSize = 64u;
        request.context.pixel = pixel;
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 64u;
        request.layout.pushConstantSizeBytes = 128u;
        request.useCache = false;
        const auto replay = serializer.Deserialize(serializer.Serialize(request));
        const auto original = Recompile(request);
        auto identity = request;
        identity.context.pixel->targetExportMapping[target] = 0xe4u;
        require(original.spirv != Recompile(identity).spirv, "the non-identity pixel export mapping did not affect the compiled shader");
        require(replay.request.context.pixel.has_value() && replay.request.context.pixel->targetExportMapping == pixel.targetExportMapping, "replay lost the non-identity pixel export mapping");
        const auto replayed = Recompile(replay.request);
        verifyResult(original, replayed);
        request.useCache = true;
        const auto cached = Recompile(request);
        const auto cachedReplay = serializer.Deserialize(serializer.Serialize(request));
        const auto hit = Recompile(cachedReplay.request);
        require(hit.cacheHit && hit.variantId == cached.variantId, "pixel replay did not reuse the original shader variant");
        verifyResult(cached, hit);
    }
}

std::vector<std::uint32_t> noPerspectiveLocations(std::span<const std::uint32_t> code) {
    using namespace ShaderRecompiler;
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = twoParameterPixel();
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto result = Recompile(request);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, std::uint32_t> locations;
    std::vector<std::uint32_t> decorated;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        if (static_cast<spv::Op>(words[at] & 0xffffu) != spv::OpDecorate) continue;
        if (words[at + 2] == spv::DecorationLocation) locations[words[at + 1]] = words[at + 3];
        if (words[at + 2] == spv::DecorationNoPerspective) decorated.push_back(words[at + 1]);
    }
    std::vector<std::uint32_t> result2;
    for (const auto id : decorated) result2.push_back(locations.count(id) != 0 ? locations.at(id) : 0xffffffffu);
    return result2;
}

void verifyPixelInputs() {
    using namespace ShaderRecompiler;
    require(PixelInputVgpr(0x326u, PixelInput::PerspectiveCentroid) == 2u && PixelInputVgpr(0x326u, PixelInput::LinearCenter) == 4u && PixelInputVgpr(0x326u, PixelInput::PositionX) == 6u, "the SPI_PS_INPUT_ADDR layout moved the inputs");
    require(PixelInputVgpr(0x7afu, PixelInput::PerspectiveCentroid) == 4u && PixelInputVgpr(0x7afu, PixelInput::PositionX) == 12u && PixelInputVgpr(0x7afu, PixelInput::PositionZ) == 14u, "ADDR-only inputs did not reserve their VGPRs");

    static constexpr std::array<std::uint32_t, 7> byPair{0xc8100000u, 0xc8110001u, 0xc8140402u, 0xc8150403u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    const auto linear = noPerspectiveLocations(byPair);
    require(linear.size() == 1u && linear[0] == 1u, "only the parameter interpolated through the linear pair must be NoPerspective");
    static constexpr std::array<std::uint32_t, 7> bothPairs{0xc8100000u, 0xc8110001u, 0xc8140002u, 0xc8150003u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    expectFailure([&] { static_cast<void>(noPerspectiveLocations(bothPairs)); }, "interpolated through both a perspective and a linear I/J pair", "a parameter read through both pairs was given one interpolation");

    static constexpr std::array<std::uint32_t, 1> code{0xbf810000u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    auto pixel = twoParameterPixel();
    pixel.inputAddr |= PixelInputBit(PixelInput::PerspectiveCentroid) | PixelInputBit(PixelInput::LinearCentroid);
    pixel.perspectiveCentroid = true;
    pixel.linearCentroid = true;
    request.context.pixel = pixel;
    const auto replay = RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(request));
    const auto& back = *replay.request.context.pixel;
    require(back.inputAddr == pixel.inputAddr && back.perspectiveCentroid && back.linearCentroid && back.noPerspective, "the pixel input layout did not survive serialization");
    std::vector<std::uint64_t> key;
    RecompileCacheKey::Build(request, key);
    const auto first = key;
    for (const auto change : {0, 1, 2}) {
        auto other = request;
        auto changed = pixel;
        if (change == 0) changed.inputAddr |= PixelInputBit(PixelInput::PerspectiveSample);
        if (change == 1) changed.perspectiveCentroid = false;
        if (change == 2) changed.linearCentroid = false;
        other.context.pixel = changed;
        RecompileCacheKey::Build(other, key);
        require(key != first && RecompileCacheKey::ContextHash(request) != RecompileCacheKey::ContextHash(other), "the cache keys ignore the pixel input layout");
    }
}

ShaderRecompiler::RecompileResult recompileSlots(std::initializer_list<std::uint32_t> controls, std::span<const std::uint32_t> code) {
    using namespace ShaderRecompiler;
    ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = static_cast<std::uint32_t>(controls.size());
    std::uint32_t index = 0;
    for (const auto control : controls) pixel.interpolatorSettings[index++] = control;
    pixel.inputAddr = PixelInputBit(PixelInput::PerspectiveCenter);
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping[0] = 0xe4u;
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = true;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return Recompile(request);
}

std::vector<std::pair<std::uint32_t, bool>> slotInputs(std::initializer_list<std::uint32_t> controls, std::span<const std::uint32_t> code) {
    const auto result = recompileSlots(controls, code);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, std::uint32_t> locations;
    std::map<std::uint32_t, bool> perVertex;
    std::vector<std::uint32_t> inputs;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
        if (op == spv::OpVariable && words[at + 3] == spv::StorageClassInput) inputs.push_back(words[at + 2]);
        if (op == spv::OpDecorate && words[at + 2] == spv::DecorationLocation) locations[words[at + 1]] = words[at + 3];
        if (op == spv::OpDecorate && words[at + 2] == spv::DecorationPerVertexKHR) perVertex[words[at + 1]] = true;
    }
    std::vector<std::pair<std::uint32_t, bool>> located;
    for (const auto id : inputs) {
        if (locations.contains(id)) located.emplace_back(locations.at(id), perVertex.contains(id));
    }
    std::sort(located.begin(), located.end());
    return located;
}

void verifyPixelParameterSlots() {
    static constexpr std::array<std::uint32_t, 7> shared{0xc8100000u, 0xc8110001u, 0xc8140500u, 0xc8150501u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    auto inputs = slotInputs({0x3u, 0x3u}, shared);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second, "inputs reading one slot were not declared once at the slot");
    inputs = slotInputs({0x404u, 0x0u}, shared);
    require(inputs.size() == 2u && inputs[0].first == 0u && inputs[1].first == 4u, "inputs of different slots moved");
    require(slotInputs({0x20u, 0x2320u}, shared).empty(), "a defaulted input was declared as a parameter");
    static constexpr std::array<std::uint32_t, 8> mixed{0xc8100000u, 0xc8110001u, 0xc8160402u, 0xc81a0802u, 0xc81e0f02u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    inputs = slotInputs({0x0u, 0x400u, 0x22u, 0x320u}, mixed);
    require(inputs.size() == 1u && inputs[0].first == 0u && inputs[0].second, "a slot read flat and interpolated did not become one per-vertex input");
    static constexpr std::array<std::uint32_t, 7> vertices{0xc8120002u, 0xc8160000u, 0xc81a0001u, 0xc81e0302u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    const auto subtracts = [](std::initializer_list<std::uint32_t> controls) {
        const auto result = recompileSlots(controls, vertices);
        const auto& words = result.spirv.Words();
        std::size_t count = 0;
        for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) count += (words[at] & 0xffffu) == spv::OpFSub;
        return count;
    };
    inputs = slotInputs({0x423u}, vertices);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second, "a pass-through input (OFFSET bit 5 with FLAT_SHADE) was not read per vertex at its slot");
    require(subtracts({0x423u}) == 0u, "v_interp_mov p10/p20 of a pass-through input subtracted vertex 0");
    inputs = slotInputs({0x403u}, vertices);
    require(inputs.size() == 1u && inputs[0].first == 3u && inputs[0].second && subtracts({0x403u}) == 2u, "v_interp_mov p10/p20 of a flat input did not read differences to vertex 0");
    require(slotInputs({0x23u}, vertices).empty(), "a defaulted input (OFFSET bit 5 without FLAT_SHADE) was declared as a parameter");
    expectFailure([] { static_cast<void>(recompileSlots({0x423u, 0x3u}, shared)); }, "passes its vertices through unchanged", "an interpolated pass-through input was accepted");
}

}

void verifyComputedTexelOffsets() {
    using namespace ShaderRecompiler;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Type2D = 9;
    struct alignas(256) Texture { std::array<std::uint8_t, 256> bytes{}; };
    static Texture texture;
    static std::array<std::uint32_t, 64> output{};
    const auto textureBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture.bytes.data()));
    const auto outputBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output.data()));
    const std::array<std::uint32_t, 16> userData{
        static_cast<std::uint32_t>(textureBase >> 8u), static_cast<std::uint32_t>((textureBase >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (Type2D << 28u), 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        static_cast<std::uint32_t>(outputBase), static_cast<std::uint32_t>((outputBase >> 32u) & 0xffffu), 64u, 0xfacu};
    const auto program = [](std::uint32_t offsetSource, std::uint32_t literal) {
        std::vector<std::uint32_t> code{0x7e020200u | offsetSource};
        if (offsetSource == 0xffu) code.push_back(literal);
        code.insert(code.end(), {0x7e040280u, 0x7e060280u, 0xf0dc0f08u, 0x00400401u, 0xe0700000u, 0x80030400u, 0xbf810000u});
        return code;
    };
    const auto computed = program(0x100u, 0u);
    const auto constant = program(0xffu, 0x3fu | (1u << 8u));
    const std::array<std::uint32_t, 2> withGather{1u, static_cast<std::uint32_t>(spv::CapabilityImageGatherExtended)};
    const auto recompile = [&](const std::vector<std::uint32_t>& code, bool offsets) {
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x30000u, code, 0, {}};
        request.context.waveSize = 32;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = withGather;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.target.nonConstantImageOffsets = offsets;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        AgcDriver::ShaderMemory memory({});
        static_cast<void>(memory.Capture(request));
        request.context.memory = memory.Regions();
        return Recompile(request).spirv;
    };
    const auto sampleOperands = [](const std::vector<std::uint32_t>& words) {
        std::uint32_t mask = 0;
        bool gatherExtended = false;
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto count = words[cursor] >> 16u;
            require(count != 0 && count <= words.size() - cursor, "texel offsets: truncated SPIR-V instruction");
            const auto op = words[cursor] & 0xffffu;
            if (op == spv::OpCapability && words[cursor + 1] == spv::CapabilityImageGatherExtended) gatherExtended = true;
            if (op == spv::OpImageSampleExplicitLod && count > 5u) mask |= words[cursor + 5];
            cursor += count;
        }
        return std::pair{mask, gatherExtended};
    };
    expectFailure([&] { static_cast<void>(recompile(computed, false)); }, "texel offset that is not a constant", "texel offsets: a computed offset compiled without maintenance8");
    const auto [computedMask, computedGather] = sampleOperands(recompile(computed, true));
    require((computedMask & spv::ImageOperandsOffsetMask) != 0u && (computedMask & spv::ImageOperandsConstOffsetMask) == 0u && computedGather, "texel offsets: a computed offset is not an Offset operand");
    for (const bool offsets : {false, true}) {
        const auto [constantMask, constantGather] = sampleOperands(recompile(constant, offsets));
        require((constantMask & spv::ImageOperandsConstOffsetMask) != 0u && (constantMask & spv::ImageOperandsOffsetMask) == 0u && !constantGather, "texel offsets: a constant offset is not a ConstOffset operand");
    }
}

void verifyUnnormalizedSamplers() {
    using namespace ShaderRecompiler;
    constexpr std::uint32_t Format8888UNorm = 56;
    constexpr std::uint32_t Format11_11_10UInt = 34;
    constexpr std::uint32_t Format32Float = 22;
    constexpr std::uint32_t Type2D = 9;
    constexpr std::uint32_t Type3D = 10;
    constexpr std::uint32_t TypeCube = 11;
    constexpr std::uint32_t Type2DArray = 13;
    struct alignas(256) Texture { std::array<std::uint8_t, 4096> bytes{}; };
    static Texture texture;
    static std::array<std::uint32_t, 64> output{};
    const auto textureBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture.bytes.data()));
    const auto outputBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output.data()));
    const std::array<std::uint32_t, 4> unnormalized{0x00008092u, 0x00fff000u, 0x05500000u, 0u};
    const std::array<std::uint32_t, 4> normalized{0x00000092u, 0x00fff000u, 0x05500000u, 0u};
    const auto imageData = [&](const std::array<std::uint32_t, 4>& sampler, std::uint32_t type, std::uint32_t format, std::uint32_t depth) {
        return std::array<std::uint32_t, 16>{
            static_cast<std::uint32_t>(textureBase >> 8u), static_cast<std::uint32_t>((textureBase >> 40u) & 0xffu) | (format << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (type << 28u), depth, 0u, 0u, 0u,
            sampler[0], sampler[1], sampler[2], sampler[3],
            static_cast<std::uint32_t>(outputBase), static_cast<std::uint32_t>((outputBase >> 32u) & 0xffffu), 64u, 0xfacu};
    };
    const auto userData = [&](const std::array<std::uint32_t, 4>& sampler) {
        return imageData(sampler, Type2D, Format8888UNorm, 0u);
    };
    const auto program = [](std::uint32_t mimg) {
        return std::vector<std::uint32_t>{0x7e020280u, 0x7e040280u, 0x7e060280u, 0x7e080280u, 0x7e0a0280u, 0x7e0c0280u, mimg, 0x00400801u, 0xe0700000u, 0x80030800u, 0xbf810000u};
    };
    const std::array<std::uint32_t, 3> capabilities{1u, static_cast<std::uint32_t>(spv::CapabilityImageGatherExtended), static_cast<std::uint32_t>(spv::CapabilityMinLod)};
    std::uint64_t nextAddress = 0x40000u;
    const auto recompile = [&](const std::vector<std::uint32_t>& code, const std::array<std::uint32_t, 16>& data, std::uint64_t address = 0u) {
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, address != 0u ? address : (nextAddress += 0x1000u), code, 0, {}};
        request.context.waveSize = 32;
        request.context.userDataBaseRegister = 0;
        request.context.userData = data;
        request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = capabilities;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;
        AgcDriver::ShaderMemory memory({});
        static_cast<void>(memory.Capture(request));
        request.context.memory = memory.Regions();
        return Recompile(request);
    };
    const auto flags = [](const RecompileResult& result, DescriptorRole role) {
        for (const auto& binding : result.bindings) {
            if (binding.role == role) return role == DescriptorRole::GuestSamplers ? binding.samplerUnnormalized : binding.imageUnnormalized;
        }
        throw std::runtime_error("unnormalized samplers: the program has no sampler or image binding");
    };
    const auto proven = [&](const RecompileResult& result, bool expected) {
        return flags(result, DescriptorRole::GuestSamplers) == std::vector<bool>{expected} && flags(result, DescriptorRole::GuestImages) == std::vector<bool>{expected};
    };
    const auto auditSpirv = [](const std::vector<std::uint32_t>& words) {
        std::size_t samples = 0;
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto count = words[cursor] >> 16u;
            require(count != 0 && count <= words.size() - cursor, "unnormalized samplers: truncated SPIR-V instruction");
            const auto op = words[cursor] & 0xffffu;
            require(op != spv::OpImageSampleImplicitLod && op != spv::OpImageSampleDrefImplicitLod && op != spv::OpImageSampleDrefExplicitLod && op != spv::OpImageGather && op != spv::OpImageDrefGather && op != spv::OpImageQueryLod, "unnormalized samplers: the SPIR-V samples in a form an unnormalized sampler does not allow");
            if (op == spv::OpImageSampleExplicitLod) {
                require(count == 7u && words[cursor + 5] == spv::ImageOperandsLodMask, "unnormalized samplers: an explicit-LOD sample has operands other than Lod");
                ++samples;
            }
            cursor += count;
        }
        require(samples != 0u, "unnormalized samplers: the SPIR-V does not sample");
    };

    const auto lz = program(0xf09c0f08u);
    const auto accepted = recompile(lz, userData(unnormalized), 0x40000u);
    require(proven(accepted, true), "unnormalized samplers: image_sample_lz through an unnormalized S# was not flagged");
    auditSpirv(accepted.spirv.Words());
    const auto plain = recompile(lz, userData(normalized), 0x40000u);
    require(proven(plain, false), "unnormalized samplers: a normalized S# was flagged");
    require(plain.variantId == accepted.variantId && plain.spirv.Words() == accepted.spirv.Words(), "unnormalized samplers: the unnormalized S# compiled another variant");

    for (const std::uint32_t mimg : {0xf0900f08u, 0xf0800f08u, 0xf0940f08u, 0xf0840f08u}) {
        const auto result = recompile(program(mimg), userData(unnormalized));
        require(proven(result, true), "unnormalized samplers: a compute image_sample_l, image_sample, image_sample_b or image_sample_cl was not flagged");
        auditSpirv(result.spirv.Words());
    }

    const auto reject = [&](std::uint32_t mimg, const std::array<std::uint32_t, 16>& data, const char* reason, const char* what) {
        expectFailure([&] { static_cast<void>(recompile(program(mimg), data)); }, reason, what);
    };
    reject(0xf0c00f08u, userData(unnormalized), "unnormalized guest sampler is used with a texel offset, which is not implemented", "unnormalized samplers: image_sample_o was accepted");
    reject(0xf0bc0f08u, imageData(unnormalized, Type2D, Format32Float, 0u), "unnormalized guest sampler is used with depth comparison, which is not implemented", "unnormalized samplers: image_sample_c_lz was accepted");
    reject(0xf11c0108u, userData(unnormalized), "unnormalized guest sampler is used by a gather, which is not implemented", "unnormalized samplers: image_gather4_lz was accepted");
    reject(0xf1800308u, userData(unnormalized), "unnormalized guest sampler is used by image_get_lod, which is not implemented", "unnormalized samplers: image_get_lod was accepted");
    reject(0xf0880f08u, userData(unnormalized), "unnormalized guest sampler is used by a sample with derivatives, which is not implemented", "unnormalized samplers: image_sample_d was accepted");
    reject(0xf0800f09u, userData(unnormalized), "unnormalized guest sampler is used by an image_sample_*_a variant, which is not implemented", "unnormalized samplers: image_sample_a was accepted");
    reject(0xf09c0f18u, imageData(unnormalized, TypeCube, Format8888UNorm, 5u), "unnormalized guest sampler samples a 1D-array, 2D-array, 3D, cube or multisampled image", "unnormalized samplers: a cube T# was accepted");
    reject(0xf09c0f10u, imageData(unnormalized, Type3D, Format8888UNorm, 3u), "unnormalized guest sampler samples a 1D-array, 2D-array, 3D, cube or multisampled image", "unnormalized samplers: a 3D T# was accepted");
    reject(0xf09c0f28u, imageData(unnormalized, Type2DArray, Format8888UNorm, 5u), "unnormalized guest sampler samples a 1D-array, 2D-array, 3D, cube or multisampled image", "unnormalized samplers: a 2D-array sample was accepted");
    reject(0xf09c0f08u, imageData(unnormalized, Type2D, Format11_11_10UInt, 0u), "unnormalized guest sampler samples an image that needs a format conversion or packed access", "unnormalized samplers: a T# with a format conversion was accepted");
    for (const std::uint32_t mimg : {0xf0c00f08u, 0xf11c0108u, 0xf09c0f18u}) {
        static_cast<void>(recompile(program(mimg), imageData(normalized, mimg == 0xf09c0f18u ? TypeCube : Type2D, Format8888UNorm, mimg == 0xf09c0f18u ? 5u : 0u)));
    }

    const std::array<std::uint32_t, 5> pixelCode{0xf0800f08u, 0x00400801u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};
    const auto pixelData = userData(unnormalized);
    RecompileRequest pixel{};
    pixel.shader = {ShaderStage::Fragment, 0x4f000u, pixelCode, 0, {}};
    pixel.context.waveSize = 64;
    pixel.context.userDataBaseRegister = 0;
    pixel.context.userData = pixelData;
    pixel.context.pixel = twoParameterPixel();
    pixel.target.vulkanVersion = 0x00401000u;
    pixel.target.spirvVersion = 0x00010300u;
    pixel.target.subgroupSize = 64;
    pixel.layout.pushConstantSizeBytes = 128;
    pixel.useCache = false;
    expectFailure([&] { static_cast<void>(Recompile(pixel)); }, "unnormalized guest sampler is used by an implicit-LOD sample, which is not implemented", "unnormalized samplers: a pixel image_sample was accepted");
}

void verifyUnusedUnnormalizedSampler() {
    using namespace ShaderRecompiler;
    ImageResource image{};
    image.resourceClass = ImageResourceClass::Sampled;
    image.numericClass = IrTextureNumericClass::Float;
    image.dimension = RdnaImageDimension::Dim2D;
    image.read = true;
    ShaderInfo info;
    info.images = {image};
    info.samplers = {SamplerResource{}};
    info.sampledPairs = {{0u, 0u, 0u}};
    ResourceSnapshot snapshot;
    snapshot.images = {DescriptorValue{{0x00001000u, 0x03800000u, 0x0000c000u, 0x90000facu, 0u, 0u, 0u, 0u}, 8u}};
    snapshot.samplers = {DescriptorValue{{0x00008092u, 0x00fff000u, 0x05500000u, 0u}, 4u}};
    const auto populate = [&](const ShaderInfo& shader) {
        BindingAllocationResult allocation;
        allocation.layout.descriptors = {{DescriptorBindingForImage(shader.images[0]), {0u}}, {DescriptorBindingKind::Samplers, {0u}}};
        DescriptorBindingBuilder{}.Populate(allocation, shader, IrShaderStage::Compute, 0u, snapshot, {});
        return allocation.bindings;
    };
    const auto bindings = populate(info);
    require(bindings.size() == 2u && bindings[0].imageUnnormalized == std::vector<bool>{true} && bindings[1].samplerUnnormalized == std::vector<bool>{true}, "unnormalized samplers: an S# without live uses was not flagged");
    auto selected = info;
    selected.images[0].tableView = 0u;
    expectFailure([&] { static_cast<void>(populate(selected)); }, "unnormalized guest sampler samples an image selected at run time, which is not implemented", "unnormalized samplers: an image table root was accepted");
    auto compared = info;
    compared.samplers[0].depthCompare = true;
    expectFailure([&] { static_cast<void>(populate(compared)); }, "unnormalized guest sampler is used with depth comparison, which is not implemented", "unnormalized samplers: a depth-compare S# without live uses was accepted");
    snapshot.samplers[0].dwords[0] = 0x00000092u;
    const auto normalized = populate(info);
    require(normalized[0].imageUnnormalized == std::vector<bool>{false} && normalized[1].samplerUnnormalized == std::vector<bool>{false}, "unnormalized samplers: a normalized S# was flagged");
}

void verifyWaveUniformValues() {
    using namespace ShaderRecompiler;
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarAddress;
    MemoryInfo global;
    global.kind = ResourceKind::Global;
    program.Resources().memoryInfo = {scalar, global};
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    const auto emit = [&](IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments, std::uint64_t flags = 0) -> IrValue& {
        auto& value = program.CreateValue(opcode, type, flags);
        for (auto* argument : arguments) value.AddArgument(argument);
        block.AppendInstruction(&value);
        return value;
    };
    const auto memory = [](std::uint32_t index) {
        MemoryFlags flags{index, 0u};
        std::uint64_t bits = 0;
        std::memcpy(&bits, &flags, sizeof(flags));
        return bits;
    };
    auto& zero = program.CreateValue(IrOpcode::Void, IrType::U32);
    zero.SetImmediateU32(0u);
    auto& active = program.CreateValue(IrOpcode::Void, IrType::U1);
    active.SetImmediateBool(true);
    auto& userData = emit(IrOpcode::GetUserData, IrType::U32, {&zero});
    auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
    auto& uniformSum = emit(IrOpcode::IAdd32, IrType::U32, {&userData, &userData});
    auto& laneSum = emit(IrOpcode::IAdd32, IrType::U32, {&userData, &lane});
    auto& address = emit(IrOpcode::GetAddressResource, IrType::AddressResource, {&userData, &userData});
    auto& scalarLoad = emit(IrOpcode::LoadAddressU32, IrType::U32, {&address, &uniformSum, &zero, &active}, memory(0u));
    auto& laneOffsetLoad = emit(IrOpcode::LoadAddressU32, IrType::U32, {&address, &laneSum, &zero, &active}, memory(0u));
    auto& globalLoad = emit(IrOpcode::LoadAddressU32, IrType::U32, {&address, &uniformSum, &zero, &active}, memory(1u));
    auto& fromScalarLoad = emit(IrOpcode::IAdd32, IrType::U32, {&scalarLoad, &uniformSum});
    auto& fromGlobalLoad = emit(IrOpcode::IAdd32, IrType::U32, {&globalLoad, &uniformSum});
    auto& compare = emit(IrOpcode::ULessThan32, IrType::U1, {&laneSum, &userData});
    auto& ballot = emit(IrOpcode::Ballot, IrType::U32x4, {&compare});
    const auto uniform = WaveUniformValues(program);
    for (const auto* value : {&userData, &uniformSum, &address, &scalarLoad, &fromScalarLoad, &ballot}) {
        require(uniform.contains(value), "wave-uniform values: a value every lane of the wave computes alike was not found uniform");
    }
    for (const auto* value : {&lane, &laneSum, &laneOffsetLoad, &globalLoad, &fromGlobalLoad, &compare}) {
        require(!uniform.contains(value), "wave-uniform values: a value that may differ between lanes was found uniform");
    }
}

void verifyTwoLaneUniformValues() {
    using namespace ShaderRecompiler;
    struct alignas(4096) GuestTables {
        std::array<std::uint32_t, 64> output{};
        std::array<std::uint32_t, 8> srt{};
    };
    static GuestTables guest;
    auto& output = guest.output;
    const auto outputBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output.data()));
    auto& srt = guest.srt;
    srt = {6u, 7u, 0u, 0u, static_cast<std::uint32_t>(outputBase), static_cast<std::uint32_t>((outputBase >> 32u) & 0xffffu), 64u, 0xfacu};
    const auto srtAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(srt.data()));
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    const std::array<std::uint32_t, 10> code{0xf4040080u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xbf8cc07fu, 0x93040302u, 0x4a020004u, 0xe0700000u, 0x80020100u, 0xbf810000u};
    const auto multiplies = [&](std::uint32_t subgroupSize) {
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x40000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = subgroupSize;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(request);
        request.context.memory = memory.Regions();
        const auto words = Recompile(request, *capture)->spirv;
        std::size_t count = 0;
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto length = words[cursor] >> 16u;
            require(length != 0 && length <= words.size() - cursor, "two-lane uniform values: truncated SPIR-V instruction");
            if ((words[cursor] & 0xffffu) == spv::OpIMul) ++count;
            cursor += length;
        }
        return count;
    };
    const auto oneLane = multiplies(64u);
    require(oneLane != 0u, "two-lane uniform values: the scalar multiply is missing from the module");
    require(multiplies(32u) == oneLane, "two-lane uniform values: a two-lane invocation computes a scalar value once per lane");
}

void verifyBdaReadFallbackFunctions() {
    using namespace ShaderRecompiler;
    const std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    const std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    alignas(256) static std::array<std::uint32_t, 32> output{};
    const auto outputAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output.data()));
    const std::array<std::uint32_t, 8> userData{0x10000000u, 0u, 0u, 0u, static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu), 128u, 0x01016facu};
    const auto compile = [&](std::uint32_t loads, bool barrier, bool coherent) {
        std::vector<std::uint32_t> code{0x7e020200u, 0x7e040201u};
        for (std::uint32_t load = 0; load < loads; ++load) {
            code.push_back(0xdc308000u | (coherent ? 0x10000u : 0u) | (4u * load + 4u));
            code.push_back(((3u + load) << 24u) | 0x007d0001u);
        }
        code.push_back(0xbf8c3f70u);
        for (std::uint32_t load = 1; load < loads; ++load) code.push_back(0x4a060103u | ((3u + load) << 9u));
        if (barrier) code.push_back(0xbf8a0000u);
        code.insert(code.end(), {0xe0700000u, 0x80010300u, 0xbf810000u});
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x50000u, code, 0, {}};
        request.context.waveSize = 32;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.bdaAbiVersion = BdaAbi::Version;
        request.target.supportedCapabilities = capabilities;
        request.target.supportedExtensions = extensions;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        return Recompile(request).spirv;
    };
    for (const bool barrier : {false, true}) {
        for (const bool coherent : {false, true}) {
            const auto one = compile(1u, barrier, coherent);
            const auto five = compile(5u, barrier, coherent);
            std::map<std::uint32_t, std::string> names;
            std::map<std::string, std::uint32_t> definitions;
            const auto reader = std::string("read_bda_dword_bytes") + (barrier ? "" : "_stop") + (coherent ? "_coherent" : "");
            std::string function;
            std::size_t compareExchanges = 0;
            std::size_t mainLookups = 0;
            std::array<std::size_t, 2> readerLoads{};
            for (std::size_t cursor = 5; cursor < five.size();) {
                const auto length = five[cursor] >> 16u;
                require(length != 0 && length <= five.size() - cursor, "BDA read functions: truncated SPIR-V instruction");
                const auto op = five[cursor] & 0xffffu;
                if (op == spv::OpName) names[five[cursor + 1]] = reinterpret_cast<const char*>(&five[cursor + 2]);
                if (op == spv::OpFunction) {
                    function = names[five[cursor + 2]];
                    if (function == "record_bda_fault" || function.starts_with("read_bda_dword_bytes")) {
                        require((five[cursor + 3] & spv::FunctionControlDontInlineMask) != 0u, "BDA read functions: a fault or byte read function may be inlined");
                        ++definitions[function];
                    }
                }
                if (op == spv::OpAtomicCompareExchange) ++compareExchanges;
                if (op == spv::OpFunctionCall && function == "main" && names[five[cursor + 3]] == "get_bda_pointer") ++mainLookups;
                if (op == spv::OpLoad && function == reader) ++readerLoads[length > 4u && (five[cursor + 4] & spv::MemoryAccessVolatileMask) != 0u];
                cursor += length;
            }
            require(definitions["record_bda_fault"] == 1u && definitions[reader] == 1u, "BDA read functions: the fault and byte read functions are not defined");
            for (const auto& [name, count] : definitions) require(count == 1u, "BDA read functions: a function is defined twice");
            require(compareExchanges == 1u, "BDA read functions: a fault is recorded outside record_bda_fault");
            require(mainLookups == 0u, "BDA read functions: a read site looks up its bytes inline");
            require(readerLoads[coherent] == 4u && readerLoads[!coherent] == 0u, "BDA read functions: the byte loads do not keep the access's coherence");
            require(five.size() - one.size() < 4u * 300u, "BDA read functions: a read site takes 300 SPIR-V words or more");
        }
    }
}

void verifyFaultSlotClaimedFirst() {
    using namespace ShaderRecompiler;
    const std::array<std::uint32_t, 6> code{0xdc308000u, 0x01000000u, 0xbf8c3f70u, 0xdc708100u, 0x00000100u, 0xbf810000u};
    const std::array<std::uint32_t, 2> userData{0x10000u, 0u};
    const std::array<std::uint32_t, 4> capabilities{spv::CapabilityShader, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    const std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const std::array<MemoryRegion, 1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}};
    request.context.waveSize = 32;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.context.memory = regions;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 32;
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto words = Recompile(request).spirv;
    std::map<std::uint32_t, std::uint32_t> constants;
    std::vector<std::uint32_t> firstWords;
    std::size_t claims = 0;
    std::size_t block = 0;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto length = words[cursor] >> 16u;
        require(length != 0 && length <= words.size() - cursor, "fault slot claim: truncated SPIR-V instruction");
        const auto op = words[cursor] & 0xffffu;
        if (op == spv::OpConstant && length == 4u) constants[words[cursor + 2]] = words[cursor + 3];
        if (op == spv::OpAccessChain && std::all_of(words.begin() + static_cast<std::ptrdiff_t>(cursor + 4), words.begin() + static_cast<std::ptrdiff_t>(cursor + length), [&](std::uint32_t index) { return constants.contains(index) && constants.at(index) == 0u; })) firstWords.push_back(words[cursor + 2]);
        if (op == spv::OpLabel) block = cursor + length;
        if (op == spv::OpAtomicCompareExchange && std::find(firstWords.begin(), firstWords.end(), words[cursor + 3]) != firstWords.end()) {
            ++claims;
            for (auto at = block; at < cursor; at += words[at] >> 16u) {
                require((words[at] & 0xffffu) == spv::OpAccessChain, "fault slot claim: a BDA fault record computes its fault words before it claims the fault slot");
            }
        }
        cursor += length;
    }
    require(claims != 0u, "fault slot claim: a global load and store record no BDA fault");
}

void verifyFunctionLdsBound() {
    using namespace ShaderRecompiler;
    const auto build = [](const auto& body) {
        IrProgram program;
        program.Resources().stage = IrShaderStage::Pixel;
        for (const std::uint32_t offset : {0u, 256u, 512u, 0xfffffff0u}) {
            MemoryInfo lds;
            lds.kind = ResourceKind::Lds;
            lds.offset = offset;
            program.Resources().memoryInfo.push_back(lds);
        }
        auto& block = program.CreateBlock();
        program.SetEntryBlock(block);
        program.BlockOrder().push_back(&block);
        const auto emit = [&](IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments, std::uint32_t memory = ~0u) -> IrValue& {
            std::uint64_t bits = 0;
            if (memory != ~0u) {
                MemoryFlags flags{memory, 0u};
                std::memcpy(&bits, &flags, sizeof(flags));
            }
            auto& value = program.CreateValue(opcode, type, bits);
            for (auto* argument : arguments) value.AddArgument(argument);
            block.AppendInstruction(&value);
            return value;
        };
        const auto constant = [&](std::uint32_t immediate) -> IrValue& {
            auto& value = program.CreateValue(IrOpcode::Void, IrType::U32);
            value.SetImmediateU32(immediate);
            return value;
        };
        auto& active = program.CreateValue(IrOpcode::Void, IrType::U1);
        active.SetImmediateBool(true);
        body(program, block, emit, constant, active);
        return std::pair{FunctionLdsDwords(program), AnalyzeProgramRequirements(program)};
    };

    const auto [laneSlots, laneRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& address = emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&lane, &constant(2u)});
        for (const std::uint32_t memory : {0u, 1u, 2u}) emit(IrOpcode::WriteSharedU32, IrType::Void, {&address, &lane, &active}, memory);
        emit(IrOpcode::LoadSharedU32, IrType::U32, {&address, &active}, 2u);
    });
    require(laneSlots == 192u, "function LDS: lane-strided slots up to byte 767 must take 192 dwords");
    require(laneRequirements.functionLds && laneRequirements.functionLdsDwords == 192u, "function LDS: the requirements do not carry the bounded size");
    {
        std::vector<std::uint32_t> rebased;
        for (const auto& [inst, address] : laneRequirements.functionLdsAddresses) rebased.push_back(address);
        std::sort(rebased.begin(), rebased.end());
        require(rebased == std::vector<std::uint32_t>{0u, 0u, 0u, 0u}, "function LDS: lane-strided slots must be addressed without the lane term");
    }

    const auto [mixed, mixedRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& address = emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&lane, &constant(2u)});
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&address, &lane, &active}, 0u);
        emit(IrOpcode::LoadSharedU32, IrType::U32, {&constant(8u), &active}, 0u);
    });
    require(mixed == 64u && mixedRequirements.functionLdsAddresses.empty(), "function LDS: a constant address beside lane-strided ones must keep the lane term");

    const auto [strides, stridesRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& four = emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&lane, &constant(2u)});
        auto& eight = emit(IrOpcode::IMul32, IrType::U32, {&lane, &constant(8u)});
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&four, &lane, &active}, 0u);
        emit(IrOpcode::LoadSharedU32, IrType::U32, {&eight, &active}, 0u);
    });
    require(strides == 128u && stridesRequirements.functionLdsAddresses.empty(), "function LDS: two lane strides must keep the lane term");

    const auto [halfWord, halfWordRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& address = emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&lane, &constant(1u)});
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&address, &lane, &active}, 0u);
    });
    require(halfWord == 64u && halfWordRequirements.functionLdsAddresses.empty(), "function LDS: a lane stride that is not whole dwords must keep the lane term");

    const auto [summed, summedRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& scaled = emit(IrOpcode::IMul32, IrType::U32, {&constant(16u), &lane});
        auto& low = emit(IrOpcode::IAdd32, IrType::U32, {&scaled, &constant(4u)});
        auto& other = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& shifted = emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&other, &constant(4u)});
        auto& high = emit(IrOpcode::IAdd32, IrType::U32, {&constant(1024u), &shifted});
        emit(IrOpcode::WriteSharedU32x2, IrType::Void, {&low, &lane, &lane, &active}, 0u);
        emit(IrOpcode::LoadSharedU32x4, IrType::U32x4, {&high, &active}, 1u);
    });
    {
        std::vector<std::uint32_t> rebased;
        for (const auto& [inst, address] : summedRequirements.functionLdsAddresses) rebased.push_back(address);
        std::sort(rebased.begin(), rebased.end());
        require(rebased == std::vector<std::uint32_t>{4u, 1024u}, "function LDS: lane ids of separate instructions with one stride must share the rebase");
        require(summedRequirements.functionLdsDwords == 384u && summed == 576u, "function LDS: the rebased array must hold the rebased addresses");
    }

    const auto [wide, wideRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& masked = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&lane, &constant(7u)});
        auto& scaled = emit(IrOpcode::IMul32, IrType::U32, {&masked, &constant(16u)});
        auto& chosen = emit(IrOpcode::SelectU32, IrType::U32, {&active, &scaled, &constant(0x100u)});
        emit(IrOpcode::LoadSharedU32x4, IrType::U32x4, {&chosen, &active}, 0u);
    });
    require(wide == 128u, "function LDS: a 4-dword access at byte 0x100 must take 68 dwords, rounded to 128");
    require(wideRequirements.functionLdsDwords == 128u, "function LDS: the requirements do not carry the wide access's size");
    require(wideRequirements.functionLdsAddresses.empty(), "function LDS: a selected address must keep the lane term");

    const auto [unbounded, unboundedRequirements] = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        auto& user = emit(IrOpcode::GetUserData, IrType::U32, {&constant(0u)});
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&user, &user, &active}, 0u);
    });
    require(unbounded == FunctionLdsDwordLimit && unboundedRequirements.functionLdsDwords == FunctionLdsDwordLimit, "function LDS: an address without a bound must keep the full array");

    const auto wrapping = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        emit(IrOpcode::LoadSharedU32, IrType::U32, {&constant(0x20u), &active}, 3u);
    }).first;
    require(wrapping == FunctionLdsDwordLimit, "function LDS: an offset that can wrap the address must keep the full array");

    const auto loop = build([](IrProgram& program, IrBlock& block, auto& emit, auto& constant, IrValue& active) {
        auto& phi = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block.AppendInstruction(&phi);
        auto& next = emit(IrOpcode::IAdd32, IrType::U32, {&phi, &constant(4u)});
        phi.AddPhiOperand(&block, &constant(0u));
        phi.AddPhiOperand(&block, &next);
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&phi, &next, &active}, 0u);
    }).first;
    require(loop == FunctionLdsDwordLimit, "function LDS: a loop-carried address must keep the full array");

    const auto joined = build([](IrProgram& program, IrBlock& block, auto& emit, auto& constant, IrValue& active) {
        auto& phi = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block.AppendInstruction(&phi);
        phi.AddPhiOperand(&block, &constant(0x40u));
        phi.AddPhiOperand(&block, &constant(0x3fcu));
        emit(IrOpcode::WriteSharedU32, IrType::Void, {&phi, &constant(1u), &active}, 0u);
    }).first;
    require(joined == 256u, "function LDS: a phi of bounded addresses must take its largest");

    const auto unsized = build([](IrProgram&, IrBlock&, auto& emit, auto& constant, IrValue& active) {
        emit(IrOpcode::LoadShared, IrType::U32, {&constant(0u), &active}, 0u);
    }).first;
    require(unsized == FunctionLdsDwordLimit, "function LDS: an access without a known width must keep the full array");
}

int main() {
    try {
        using namespace ShaderRecompiler;
        verifyRegisterSources();
        verifyEvaluatedValues();
        verifyPureFlatSlots();
        verifyScalarBufferRule();
        verifyScalarAddressRule();
        verifyImageTablePlans();
        verifyImageTableSnapshot();
        verifyImageTableCompare();
        verifyImageTableSrgb();
        verifyImageTableReduction();
        verifyPointerImageTable();
        verifyDescriptorPhis();
        verifyProgramCounterRelativeData();
        verifyLanesOutsideHostSubgroup();
        verifyMeshConfiguration();
        verifyPixelInputs();
        verifyPixelRequestSerialization();
        verifyLegacyPixelRequests();
        verifyPixelExportReplay();
        verifyPixelParameterSlots();
        verifyComputedTexelOffsets();
        verifyUnnormalizedSamplers();
        verifyUnusedUnnormalizedSampler();
        verifyWaveUniformValues();
        verifyTwoLaneUniformValues();
        verifyBdaReadFallbackFunctions();
        verifyFaultSlotClaimedFirst();
        verifyFunctionLdsBound();
#if ANYPS5_ENABLE_SPIRV_TOOLS
        const std::vector<std::uint32_t> minimalSpirv{
            0x07230203u, 0x00010000u, 0u, 5u, 0u,
            0x00020011u, 1u,
            0x0003000eu, 0u, 1u,
            0x0005000fu, 5u, 3u, 0x6e69616du, 0u,
            0x00060010u, 3u, 17u, 1u, 1u, 1u,
            0x00020013u, 1u,
            0x00030021u, 2u, 1u,
            0x00050036u, 1u, 3u, 0u, 2u,
            0x000200f8u, 4u,
            0x00010000u,
            0x000100fdu,
            0x00010038u
        };
        const auto optimizedSpirv = ValidateAndOptimizeSpirv(minimalSpirv, 0x00401001u, 0x00010000u);
        require(optimizedSpirv.size() < minimalSpirv.size(), "SPIR-V optimization did not remove the no-op");
        require(optimizedSpirv == ValidateAndOptimizeSpirv(minimalSpirv, 0x00401001u, 0x00010000u), "SPIR-V optimization is not deterministic");
#endif
        const std::array<std::uint32_t, 8> code{0xf4040004u, 0xfa000000u, 0xf4000080u, 0xfa000000u, 0x7e000202u, 0xf80008cfu, 0u, 0xbf810000u};
        struct alignas(4096) GuestTables {
            std::uint32_t payload = 0;
            std::uint64_t table = 0;
        };
        static GuestTables guest;
        auto& payload = guest.payload;
        payload = 0x3f800000u;
        auto& table = guest.table;
        table = reinterpret_cast<std::uintptr_t>(&payload);
        const auto address = reinterpret_cast<std::uintptr_t>(&table);
        const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
        RecompileRequest request{};
        request.shader = {ShaderStage::Vertex, 0x10000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 8;
        request.context.userData = userData;
        request.context.vertex = ShaderVertexStageInfo{};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 64;
        request.target.fragmentShaderBarycentricEnabled = false;
        request.layout.pushConstantSizeBytes = 128;

        expectFailure([&] { static_cast<void>(Recompile(request)); }, "SrtWalker::EvaluateRuntimeSources", "missing snapshot unexpectedly read live memory");
        AgcDriver::ShaderMemory memory({});
        const auto capture = memory.Capture(request);
        {
            // The payload slot is consumed by the export alone: pure, its leaf traced at the
            // payload's address; the table pointer's words (the payload's address cone) are among
            // the other reads.
            const auto& pure = capture->plan->pureFlatSlots;
            require(std::count(pure.begin(), pure.end(), std::uint8_t{1}) == 1, "the payload slot is not the one pure flat slot");
            const auto& trace = capture->readTrace;
            require(trace.leaves.size() == 1 && trace.leaves[0].second == reinterpret_cast<std::uintptr_t>(&payload), "the pure slot's leaf was not traced at the payload");
            const auto slot = trace.leaves[0].first;
            require(slot < pure.size() && pure[slot] != 0 && capture->snapshot.flattenedSrt.at(slot) == payload, "the traced leaf is not the pure slot");
            require(std::find(trace.otherReads.begin(), trace.otherReads.end(), address) != trace.otherReads.end(), "the table pointer read was not traced among the other reads");
            require(std::find(trace.otherReads.begin(), trace.otherReads.end(), reinterpret_cast<std::uintptr_t>(&payload)) == trace.otherReads.end(), "the payload counts as a walk read");
        }
        auto regions = memory.Regions();
        std::size_t capturedBytes = 0;
        for (const auto& region : regions) capturedBytes += region.bytes.size();
        require(capturedBytes == sizeof(table) + sizeof(payload), "nested pointer reads were not captured");
        request.context.memory = regions;
        const auto first = Recompile(request);
        require(!first.spirv.empty(), "empty compiled shader");
        require(!first.cacheHit, "first shader compilation unexpectedly hit the cache");
        const auto plan = GetResourcePlan(request);
        require(plan == GetResourcePlan(request), "resource plan was rebuilt");
        const auto cached = Recompile(request);
        require(cached.cacheHit, "unchanged shader did not hit the cache");
        verifyResult(first, cached);
        auto relocated = request;
        relocated.shader.codeAddress += 0x1000;
        require(Recompile(relocated).cacheHit, "shader relocation caused recompilation");
        auto changedTarget = request;
        changedTarget.target.subgroupSize = 32;
        require(GetResourcePlan(changedTarget) != plan, "different target reused the source entry");
        std::vector<std::uint32_t> changedCode(code.begin(), code.end());
        changedCode.insert(changedCode.begin(), 0xbf800000u);
        auto changedSource = request;
        changedSource.shader.code = changedCode;
        require(GetResourcePlan(changedSource) != plan, "changed code reused the source entry");
        auto uncached = request;
        uncached.useCache = false;
        require(GetResourcePlan(uncached) != plan, "disabled cache reused the resource plan");
        const auto fresh = Recompile(uncached);
        require(!fresh.cacheHit, "disabled cache reused the compiled variant");
        verifyResult(first, fresh);
        require(!RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(uncached)).request.useCache, "cache policy was lost in serialization");
        auto offsets = uncached;
        offsets.target.nonConstantImageOffsets = true;
        require(RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(offsets)).request.target.nonConstantImageOffsets, "non-constant texel offsets were lost in serialization");
        auto srgb = uncached;
        srgb.target.srgbDecodeFormats = 3u;
        require(RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(srgb)).request.target.srgbDecodeFormats == 3u, "the sRGB formats decoded in the shader were lost in serialization");
        auto changedLayout = request;
        changedLayout.layout.pushConstantSizeBytes = 64;
        require(!Recompile(changedLayout).cacheHit, "binding layout change reused an incompatible variant");
        require(Recompile(changedLayout).cacheHit, "new binding layout variant was not cached");
        require(Recompile(request).cacheHit, "compiling a new variant evicted the original");
        auto missingMemory = request;
        missingMemory.context.memory = {};
        expectFailure([&] { static_cast<void>(Recompile(missingMemory)); }, "SrtWalker::EvaluateRuntimeSources", "cache hit bypassed resource validation");
#if ANYPS5_ENABLE_SPIRV_TOOLS
        auto invalidSpirv = first.spirv;
        invalidSpirv[0] = 0;
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(invalidSpirv, request.target.vulkanVersion, request.target.spirvVersion)); }, "SPIR-V validation before optimization failed", "invalid SPIR-V passed validation");
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(first.spirv, 0x00400000u, 0x00010600u)); }, "unsupported Vulkan/SPIR-V target", "incompatible target accepted");
        expectFailure([&] { static_cast<void>(ValidateAndOptimizeSpirv(first.spirv, 0x00405000u, 0x00010600u)); }, "unsupported Vulkan target", "unknown Vulkan target accepted");
#endif
        payload = 0x40000000u;
        AgcDriver::ShaderMemory updatedMemory({});
        const auto updatedCapture = updatedMemory.Capture(request);
        const auto updatedRegions = updatedMemory.Regions();
        auto updated = request;
        updated.context.memory = updatedRegions;
        const auto updatedCached = Recompile(updated);
        require(updatedCached.cacheHit, "dynamic shader data caused recompilation");
        updated.useCache = false;
        verifyResult(updatedCached, Recompile(updated));
        bool changedData = updatedCached.pushConstants != first.pushConstants;
        for (std::size_t i = 0; i < first.bindings.size(); ++i) changedData = changedData || updatedCached.bindings.at(i).guestDescriptor != first.bindings[i].guestDescriptor;
        require(changedData, "cache hit retained stale shader data");
        {
            // Two captures differing only in the payload (the pure slot) recompile to the same
            // variant, with bindings equal apart from that slot's FlattenedSrt word.
            payload = 0x40400000u;
            AgcDriver::ShaderMemory changedMemory({});
            const auto changedCapture = changedMemory.Capture(request);
            auto changed = updated;
            changed.useCache = true;
            changed.context.memory = changedMemory.Regions();
            auto before = updated;
            before.useCache = true;
            before.context.memory = updatedRegions;
            const auto first = Recompile(before, *updatedCapture);
            const auto second = Recompile(changed, *changedCapture);
            require(first->variantId == second->variantId, "a pure slot's value changed the variant");
            require(first->bindings.size() == second->bindings.size(), "a pure slot's value changed the bindings");
            const auto slot = changedCapture->readTrace.leaves.at(0).first;
            for (std::size_t i = 0; i < first->bindings.size(); ++i) {
                const auto& left = first->bindings[i];
                const auto& right = second->bindings[i];
                if (left.role != DescriptorRole::FlattenedSrt) {
                    require(left.guestDescriptor == right.guestDescriptor, "a pure slot's value changed a non-data binding");
                    continue;
                }
                require(left.guestDescriptor.size() == right.guestDescriptor.size() && left.guestDescriptor.at(slot) == 0x40000000u && right.guestDescriptor.at(slot) == 0x40400000u, "the FlattenedSrt binding does not carry the pure slot's value");
                for (std::size_t j = 0; j < left.guestDescriptor.size(); ++j) {
                    if (j != slot) require(left.guestDescriptor[j] == right.guestDescriptor[j], "the FlattenedSrt binding differs beyond the pure slot");
                }
            }
            payload = 0x40000000u;
        }
        auto concurrent = request;
        concurrent.layout.pushConstantSizeBytes = 60;
        std::array<std::future<RecompileResult>, 4> concurrentResults;
        for (auto& future : concurrentResults) future = std::async(std::launch::async, [concurrent] { return Recompile(concurrent); });
        std::uint32_t compilations = 0;
        for (auto& future : concurrentResults) {
            const auto result = future.get();
            if (!result.cacheHit) ++compilations;
        }
        require(compilations == 1, "concurrent requests compiled the same variant repeatedly");
        const auto serialized = RequestSerializer{}.Serialize(request);
        table = 0;
        payload = 0xdeadbeefu;
        verifyResult(first, Recompile(request));
        auto replay = RequestSerializer{}.Deserialize(serialized);
        verifyResult(first, Recompile(replay.request));
        RequestMemoryView view(replay.request.context.memory);
        const auto runtime = view.MakeRuntime(userData, request.shader.codeAddress);
        std::uint32_t captured = 0;
        require(runtime.readMemory(runtime.userContext, reinterpret_cast<std::uintptr_t>(&payload), &captured) && captured == 0x3f800000u, "snapshot changed with live memory");

        request.context.userDataBaseRegister = 0x8c;
        expectFailure([&] { static_cast<void>(PrepareResourceProgram(request)); }, "shader user data exceeds the scalar register bank", "PM4 register address accepted as SGPR base");
        request.context.userDataBaseRegister = 105;
        expectFailure([&] { static_cast<void>(PrepareResourceProgram(request)); }, "shader user data exceeds the scalar register bank", "user data overran scalar register bank");
        request.context.userDataBaseRegister = 8;
        request.context.memory = {};
        AgcDriver::ShaderMemory invalid({});
        expectFailure([&] { invalid.Capture(request); }, "null or misaligned address", "null nested pointer was accepted");
        std::cout << "Shader memory capture, strict validation and deterministic replay passed\n";
        return 0;
    } catch (const std::exception& error) {
        const std::string message(error.what());
        std::cerr << message.substr(0, message.find("RecompileRequest:")) << '\n';
        return 1;
    }
}

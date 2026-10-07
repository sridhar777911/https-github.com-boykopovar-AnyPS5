#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Recompiler.hpp"
#include "BdaAbi.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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

using AgcDriver::Graphics::Require;
namespace Abi = ShaderRecompiler::ImageTableAbi;

constexpr std::uint32_t PageBytes = 4096;
constexpr std::uint32_t ReservedBytes = 65536;
constexpr std::uint32_t Stride = 48;
constexpr std::uint32_t Records = 4;
constexpr std::uint32_t TableBytes = Records * Stride;
constexpr std::uint32_t TableOffset = PageBytes - 2u * Stride;
constexpr std::uint32_t ImageOffset = 16;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;

struct alignas(256) Texture {
    std::array<std::uint8_t, 256> bytes{};
};

Texture Texels;
alignas(256) std::array<std::uint32_t, 16> Srt{};
alignas(256) std::array<std::uint32_t, 4> Output{};

const std::vector<std::uint32_t> TableCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x9310b010u,
    0xf4280502u, 0x20000010u, 0xf09c8f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u};

std::uint64_t Address(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::array<std::uint32_t, 4> Image(const void* base) {
    const auto address = Address(base);
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (Type2D << 28u)};
}

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = Address(base);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0xfacu};
}

std::byte* Reserve() {
#ifdef _WIN32
    auto* base = static_cast<std::byte*>(VirtualAlloc(nullptr, ReservedBytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    auto* mapped = mmap(nullptr, ReservedBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    auto* base = mapped == MAP_FAILED ? nullptr : static_cast<std::byte*>(mapped);
#endif
    Require(base != nullptr, "image table cache: cannot reserve an inaccessible range");
    return base;
}

void Commit(std::byte* page) {
#ifdef _WIN32
    const bool committed = VirtualAlloc(page, PageBytes, MEM_COMMIT, PAGE_READWRITE) == page;
#else
    const bool committed = mprotect(page, PageBytes, PROT_READ | PROT_WRITE) == 0;
#endif
    Require(committed, "image table cache: cannot commit a page");
}

void Release(std::byte* base) {
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, ReservedBytes);
#endif
}

struct Captured {
    std::unique_ptr<AgcDriver::ShaderMemory> memory;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    AgcDriver::DriverDetail::StageCapture stage;
};

Captured Capture(std::byte* table) {
    using namespace ShaderRecompiler;
    static const std::array<std::uint32_t, 3> capabilities{29u, 5302u, 1u};
    static const std::array<std::string_view, 1> extensions{"SPV_EXT_descriptor_indexing"};
    const auto srtAddress = Address(Srt.data());
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x40000u + TableCode.size() * 4u, TableCode, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 32;
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    const auto tableV = Buffer(table, TableBytes);
    const auto outputV = Buffer(Output.data(), sizeof(Output));
    Srt = {};
    std::copy(tableV.begin(), tableV.end(), Srt.begin());
    std::copy(outputV.begin(), outputV.end(), Srt.begin() + 12);

    Captured result;
    result.memory = std::make_unique<AgcDriver::ShaderMemory>(std::span<const MemoryRegion>{});
    result.capture = result.memory->Capture(request);
    result.stage.regions = result.memory->TakeRecentRegions();
    request.context.memory = result.memory->Regions();
    result.stage.compiled = Recompile(request, *result.capture);
    result.stage.forgetSerial = 7;
    result.stage.pushOffset = 16;
    return result;
}

std::uint32_t Code(const Captured& captured, std::uint32_t record) {
    const auto& bindings = captured.stage.compiled->bindings;
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::ImageTableMap; });
    Require(binding != bindings.end(), "image table cache: the result has no image table map");
    const auto& map = binding->guestDescriptor;
    return map.at(map.at(Abi::HeaderWords + Abi::ViewMapStart) + record);
}

std::optional<std::uint32_t> WordAt(const AgcDriver::DriverDetail::DispatchVariant& variant, std::uint64_t address) {
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        if (begin <= address && address + 4u <= end) return variant.words.at(offset + (address - begin) / 4u);
        offset += (end - begin) / 4u;
    }
    return std::nullopt;
}

void SetRecord(std::byte* table, std::uint32_t record) {
    const auto words = Image(Texels.bytes.data());
    std::memcpy(table + record * Stride + ImageOffset, words.data(), sizeof(words));
}

void RunTests() {
    using AgcDriver::DriverDetail::CacheableResult;
    using AgcDriver::DriverDetail::DrawStageVariant;
    auto* reserved = Reserve();
    Commit(reserved);
    auto* table = reserved + TableOffset;
    SetRecord(table, 0);
    SetRecord(table, 1);

    const auto unmapped = Capture(table);
    const auto& poison = unmapped.stage.compiled->imageTablePoison;
    const auto unmappedCode = Code(unmapped, 2);
    Require((unmappedCode & Abi::PoisonFlag) != 0u && poison.at(unmappedCode & ~Abi::PoisonFlag).reason == static_cast<std::uint32_t>(Abi::PoisonReason::Unmapped) && Code(unmapped, 3) == unmappedCode, "image table cache: records on an uncommitted page are not Unmapped poison");
    Require((Code(unmapped, 0) & Abi::PoisonFlag) == 0u && (Code(unmapped, 1) & Abi::PoisonFlag) == 0u, "image table cache: a record on the committed page is poison");
    Require(!CacheableResult(*unmapped.stage.compiled), "image table cache: a result with Unmapped entries is cacheable");
    Require(DrawStageVariant(unmapped.stage, nullptr, std::nullopt, {}) == nullptr, "image table cache: the draw path keeps a stage variant with Unmapped entries");

    Commit(reserved + PageBytes);
    SetRecord(table, 2);
    SetRecord(table, 3);
    for (const auto& region : unmapped.stage.regions) {
        Require(std::memcmp(reinterpret_cast<const void*>(region.guestAddress), region.bytes.data(), region.bytes.size()) == 0, "image table cache: a word the Unmapped capture read changed");
    }

    const auto mapped = Capture(table);
    Require(CacheableResult(*mapped.stage.compiled), "image table cache: a result without Unmapped entries is not cacheable");
    Require(Code(mapped, 2) == Code(mapped, 0) && (Code(mapped, 2) & Abi::PoisonFlag) == 0u, "image table cache: a record on a newly committed page is not an image");

    const std::vector<std::byte> decoded{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<AgcDriver::Graphics::DecodeRead, 1> reads{{{Address(Srt.data()) - 4u, decoded}}};
    const auto variant = DrawStageVariant(mapped.stage, nullptr, ShaderRecompiler::ShaderVertexStageInfo{}, reads);
    Require(variant != nullptr && variant->compiled == mapped.stage.compiled && variant->forgetSerial == 7u && variant->pushOffset == 16u && variant->vertexInfo != nullptr, "image table cache: the draw path did not keep a cacheable stage");
    Require(std::is_sorted(variant->runs.begin(), variant->runs.end()) && WordAt(*variant, reads[0].address) == 0x04030201u, "image table cache: the stage variant's runs are not sorted with the decode reads");
    const auto image = Image(Texels.bytes.data());
    for (std::uint32_t record = 2; record < Records; ++record) {
        for (std::uint32_t dword = 0; dword < image.size(); ++dword) Require(WordAt(*variant, Address(table + record * Stride + ImageOffset + dword * 4u)) == image[dword], "image table cache: the stage variant does not hold a newly committed record");
    }
    const AgcDriver::DriverDetail::StageCapture empty{};
    Require(DrawStageVariant(empty, nullptr, std::nullopt, {}) == nullptr, "image table cache: a stage without a result was kept");
    Release(reserved);
}

}

int main() {
    try {
        RunTests();
        std::puts("image table cache tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

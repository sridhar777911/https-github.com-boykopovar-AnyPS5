#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "ImageTableAbi.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 4;
constexpr std::uint32_t Stride = 48;
constexpr std::uint32_t Records = 12;
constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t MaxGroups = 8;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Format32SInt = 21;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8_8Srgb = 129;
constexpr std::uint32_t Type2D = 9;

alignas(256) std::array<float, Width * Height> FloatTexels{};
alignas(256) std::array<float, Width * Height> OtherFloatTexels{};
alignas(256) std::array<std::uint32_t, Width * Height> UintTexels{};
alignas(256) std::array<std::int32_t, Width * Height> SintTexels{};
alignas(256) std::array<std::uint32_t, Records * Stride / 4> Table{};
alignas(256) std::array<std::uint32_t, 64> Keys{};
alignas(256) std::array<std::uint32_t, MaxGroups * 128 * 4> Output{};
alignas(256) std::array<std::uint32_t, 32> Srt{};
constexpr std::uint32_t PaletteOffset = 0x40;
constexpr std::uint32_t PaletteBytes = PaletteOffset + 256u * 32u;
std::vector<std::uint32_t> Root(0x100000u);

alignas(256) constexpr std::array<std::uint32_t, 28> TableCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu,
    0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u, 0x34101286u, 0x4a101100u,
    0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 30> TableDirectSamplerCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf4280902u, 0x24000010u, 0x360200bfu, 0x7e020d01u,
    0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u,
    0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 30> TableSamplerCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080900u, 0xfa000040u,
    0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf4280802u, 0x24000000u, 0x360200bfu, 0x7e020d01u,
    0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u,
    0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 30> TableCutCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu,
    0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0x7da400ffu, 0x00001000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u,
    0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 32> TableWave32Code{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x7e200500u, 0x90108510u,
    0x8f118202u, 0x81101110u, 0x8f108210u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u,
    0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u,
    0x01090401u, 0x7e120202u, 0x34101287u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 22> DirectCode{
    0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u, 0xf4080900u, 0xfa000040u, 0x360200bfu, 0x7e020d01u,
    0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u,
    0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 30> PointerCode{
    0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x8711ff11u, 0x000000ffu, 0x8f128511u, 0xf40c0900u, 0x24000040u, 0x360200bfu, 0x7e020d01u,
    0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c0f08u, 0x01090401u, 0x7e120202u,
    0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 32> LoadedPointerCode{
    0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0xf4040500u, 0xfa000030u,
    0x8f108202u, 0xf4200444u, 0x20000000u, 0x8711ff11u, 0x000000ffu, 0x8f128511u, 0xf40c090au, 0x241fff00u,
    0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu, 0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c0f08u,
    0x01090401u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 28> UnboundedPointerCode{
    0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x8f128511u, 0xf40c0900u, 0x24000040u, 0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu,
    0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c0f08u, 0x01090401u, 0x7e120202u, 0x34101286u, 0x4a101100u,
    0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 23> ReadTableCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e040280u, 0xf0008f08u, 0x00090401u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 25> ReadTableCutCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e040280u, 0x7da400ffu, 0x00001000u,
    0xf0008f08u, 0x00090401u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u,
    0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 23> ReadTableD16Code{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e040280u, 0xf000a388u, 0x80090401u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0701000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 27> ReadTableWave32Code{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x7e200500u, 0x90108510u,
    0x8f118202u, 0x81101110u, 0x8f108210u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u,
    0x360200bfu, 0x7e040280u, 0xf0008f08u, 0x00090401u, 0x7e120202u, 0x34101287u, 0x4a101100u, 0x34101084u,
    0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 17> ReadDirectCode{
    0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u, 0xf4080900u, 0xfa000040u, 0x360200bfu, 0x7e040280u,
    0xf0008f08u, 0x00090401u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u,
    0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 17> ReadDirectD16Code{
    0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u, 0xf4080900u, 0xfa000040u, 0x360200bfu, 0x7e040280u,
    0xf000a388u, 0x80090401u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0701000u, 0x80070408u,
    0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 25> ReadPointerCode{
    0xf4080200u, 0xfa000000u, 0xf4080700u, 0xfa000010u, 0xf4080800u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x8711ff11u, 0x000000ffu, 0x8f128511u, 0xf40c0900u, 0x24000040u, 0x360200bfu, 0x7e040280u,
    0xf0000f08u, 0x00090401u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0781000u, 0x80070408u,
    0xbf810000u,
};

constexpr std::array<std::uint32_t, 4> PointClamp{0x92u, (4u * 256u) << 12u, 0u, 0u};
constexpr std::array<std::uint32_t, 4> LinearWrap{0u, (4u * 256u) << 12u, (1u << 20u) | (1u << 22u), 0u};
constexpr std::array<std::uint32_t, 4> PointWrap{0u, (4u * 256u) << 12u, 1u << 24u, 0u};

std::array<std::uint32_t, 4> Image(const void* texels, std::uint32_t format) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u), ((Width - 1u) >> 2u) | ((Height - 1u) << 14u) | (1u << 31u), 0xfacu | (Type2D << 28u)};
}

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x00027facu};
}

void SetRecord(std::uint32_t record, const std::array<std::uint32_t, 4>& sampler, const std::array<std::uint32_t, 4>& image) {
    auto* words = Table.data() + record * (Stride / 4u);
    std::fill(words, words + Stride / 4u, 0u);
    std::copy(sampler.begin(), sampler.end(), words);
    std::copy(image.begin(), image.end(), words + 4);
}

std::uint32_t Half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (((bits >> 23u) - 112u) << 10u) | ((bits >> 13u) & 0x3ffu);
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        file = std::tmpfile();
        Require(file != nullptr, "cannot open a temporary file for stderr");
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        _dup2(_fileno(file), _fileno(stderr));
#else
        saved = dup(fileno(stderr));
        dup2(fileno(file), fileno(stderr));
#endif
    }
    std::string Finish() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::rewind(file);
        std::string text;
        char buffer[4096];
        for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, read);
        std::fclose(file);
        std::fwrite(text.data(), 1, text.size(), stderr);
        return text;
    }

private:
    std::FILE* file = nullptr;
    int saved = -1;
};

struct Outcome {
    std::vector<std::uint32_t> words;
    std::string log;
    std::uint64_t variantId = 0;
    std::uint32_t imageTableCount = 0;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::vector<std::array<std::uint32_t, 4>> samplerTable;
};

std::vector<std::array<std::uint32_t, 4>> SamplerWords(const std::vector<std::uint32_t>& words) {
    std::vector<std::array<std::uint32_t, 4>> result;
    for (std::size_t first = 0; first + 4u <= words.size(); first += 4u) result.push_back({words[first], words[first + 1u], words[first + 2u], words[first + 3u]});
    return result;
}

Outcome Execute(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> span, const void* root, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint32_t groups, std::uint32_t threads, std::uint32_t waveSize) {
    const auto rootAddress = reinterpret_cast<std::uintptr_t>(root);
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(rootAddress), static_cast<std::uint32_t>(rootAddress >> 32u)};
    std::fill(Output.begin(), Output.end(), 0xdeadbeefu);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(span.data()), span, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    const auto result = ShaderRecompiler::Recompile(request);
    Outcome outcome;
    outcome.variantId = result.variantId;
    for (const auto& binding : result.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::ImageTable) outcome.imageTableCount = binding.count;
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers) outcome.samplers = SamplerWords(binding.guestDescriptor);
        if (binding.role == ShaderRecompiler::DescriptorRole::SamplerTable) outcome.samplerTable = SamplerWords(binding.guestDescriptor);
    }
    StderrCapture capture;
    try {
        device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(span.data()));
        device.WaitIdle();
    } catch (...) {
        static_cast<void>(capture.Finish());
        throw;
    }
    outcome.log = capture.Finish();
    outcome.words.assign(Output.begin(), Output.begin() + groups * threads * 4u);
    return outcome;
}

template <std::size_t CodeWords>
Outcome Run(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, CodeWords>& code, std::uint32_t groups, std::uint32_t threads, std::uint32_t waveSize, bool outputIntoTable = false) {
    const auto table = Buffer(Table.data(), static_cast<std::uint32_t>(sizeof(Table)));
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    const auto output = outputIntoTable ? Buffer(Table.data(), static_cast<std::uint32_t>(sizeof(Table))) : Buffer(Output.data(), static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(table.begin(), table.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    std::copy(output.begin(), output.end(), Srt.begin() + 8);
    const std::span<const std::uint32_t> span(code);
    const std::array<ShaderRecompiler::MemoryRegion, 4> memory{{
        {reinterpret_cast<std::uintptr_t>(span.data()), std::as_bytes(span)},
        {reinterpret_cast<std::uintptr_t>(Srt.data()), std::as_bytes(std::span(Srt))},
        {reinterpret_cast<std::uintptr_t>(Table.data()), std::as_bytes(std::span(Table))},
        {reinterpret_cast<std::uintptr_t>(Keys.data()), std::as_bytes(std::span(Keys))},
    }};
    return Execute(device, span, Srt.data(), memory, groups, threads, waveSize);
}

template <std::size_t CodeWords>
Outcome RunPointer(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, CodeWords>& code, std::uint32_t groups) {
    auto* root = Root.data();
    const auto palette = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(root)) + PaletteOffset + 0x100u;
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    const auto output = Buffer(Output.data(), static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(keys.begin(), keys.end(), root);
    std::copy(output.begin(), output.end(), root + 4);
    std::copy(PointClamp.begin(), PointClamp.end(), root + 8);
    root[12] = static_cast<std::uint32_t>(palette);
    root[13] = static_cast<std::uint32_t>(palette >> 32u);
    const std::span<const std::uint32_t> span(code);
    const std::array<ShaderRecompiler::MemoryRegion, 3> memory{{
        {reinterpret_cast<std::uintptr_t>(span.data()), std::as_bytes(span)},
        {reinterpret_cast<std::uintptr_t>(root), std::as_bytes(std::span(root, PaletteBytes / 4u))},
        {reinterpret_cast<std::uintptr_t>(Keys.data()), std::as_bytes(std::span(Keys))},
    }};
    return Execute(device, span, root, memory, groups, Lanes, 64);
}

void SetEntry(std::uint32_t entry, const std::array<std::uint32_t, 4>& image) {
    auto* words = Root.data() + (PaletteOffset + entry * 32u) / 4u;
    std::fill(words, words + 8, 0u);
    std::copy(image.begin(), image.end(), words);
}

Outcome RunDirect(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 4>& sampler, const std::array<std::uint32_t, 4>& image) {
    std::copy(sampler.begin(), sampler.end(), Srt.begin() + 12);
    std::copy(image.begin(), image.end(), Srt.begin() + 16);
    return Run(device, DirectCode, 1, Lanes, 64);
}

std::vector<std::uint32_t> Group(const Outcome& outcome, std::uint32_t group, std::uint32_t threads = Lanes) {
    return std::vector<std::uint32_t>(outcome.words.begin() + group * threads * 4u, outcome.words.begin() + (group + 1u) * threads * 4u);
}

bool Faulted(const Outcome& outcome) {
    return outcome.log.find("image table:") != std::string::npos;
}

bool Holds(const std::vector<std::array<std::uint32_t, 4>>& samplers, const std::array<std::uint32_t, 4>& sampler) {
    return std::find(samplers.begin(), samplers.end(), sampler) != samplers.end();
}

bool TablePointCopy(const Outcome& outcome, const std::array<std::uint32_t, 4>& sampler, const std::array<std::uint32_t, 4>& point) {
    bool found = false;
    for (std::size_t entry = 0; entry + 1u < outcome.samplerTable.size(); entry += 2u) {
        if (outcome.samplerTable[entry] != sampler) continue;
        found = true;
        if (outcome.samplerTable[entry + 1u] != point) return false;
    }
    return found;
}

void Fill() {
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const auto index = y * Width + x;
            FloatTexels[index] = static_cast<float>(x) + 100.0f * static_cast<float>(y) + 0.25f;
            OtherFloatTexels[index] = -static_cast<float>(x) - 0.5f;
            UintTexels[index] = 0x1000u + x + 0x100u * y;
            SintTexels[index] = -3 * static_cast<std::int32_t>(x + 1u) - 1000 * static_cast<std::int32_t>(y);
        }
    }
}

void RequireRed(const Outcome& outcome, std::uint32_t group, std::uint32_t threads, const std::function<std::uint32_t(std::uint32_t)>& expected, const std::string& what) {
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        const auto actual = outcome.words[(group * threads + lane) * 4u];
        Require(actual == expected(lane % Width), what + ": lane " + std::to_string(lane) + " is " + Hex(actual) + ", expected " + Hex(expected(lane % Width)));
    }
}

template <std::size_t CodeWords>
void RunWaveKeys(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, CodeWords>& code, const std::string& what) {
    if (device.Target().subgroupSize != 32u) {
        std::printf("%s: per-wave key case skipped, subgroup size %u does not hold exactly one wave32\n", what.c_str(), device.Target().subgroupSize);
        return;
    }
    for (std::uint32_t wave = 0; wave < 8; ++wave) Keys[wave] = wave % 3u;
    const auto waves = Run(device, code, 2, 128, 32);
    Require(!Faulted(waves), what + ": per-wave keys faulted:\n" + waves.log);
    for (std::uint32_t group = 0; group < 2; ++group) {
        for (std::uint32_t wave = 0; wave < 4; ++wave) {
            const auto key = (group * 4u + wave) % 3u;
            for (std::uint32_t lane = 0; lane < 32; ++lane) {
                const auto thread = wave * 32u + lane;
                const auto actual = waves.words[(group * 128u + thread) * 4u];
                const auto x = thread % Width;
                const auto expected = key == 0u ? std::bit_cast<std::uint32_t>(FloatTexels[x]) : key == 1u ? UintTexels[x] : static_cast<std::uint32_t>(SintTexels[x]);
                Require(actual == expected, what + ": wave " + std::to_string(wave) + " of group " + std::to_string(group) + " read the wrong entry at lane " + std::to_string(lane));
            }
        }
    }
}

void RunTests(AgcDriver::VulkanDevice& device) {
    Fill();
    const auto floatImage = Image(FloatTexels.data(), Format32Float);
    const auto uintImage = Image(UintTexels.data(), Format32UInt);
    const auto sintImage = Image(SintTexels.data(), Format32SInt);
    SetRecord(0, PointClamp, floatImage);
    SetRecord(1, PointClamp, uintImage);
    SetRecord(2, PointClamp, sintImage);
    SetRecord(3, LinearWrap, floatImage);
    SetRecord(4, LinearWrap, uintImage);
    SetRecord(5, LinearWrap, sintImage);
    SetRecord(6, {0u, 0u, 0u, 0u}, {0u, 0u, 0u, 0u});
    SetRecord(7, PointClamp, {0x1234u, 0x5678u, 0x9abcu, 0x0000ffacu});
    for (std::uint32_t record = 8; record < Records; ++record) SetRecord(record, PointClamp, floatImage);

    Keys = {};
    for (std::uint32_t group = 0; group < 6; ++group) Keys[group] = group;
    const auto classes = Run(device, TableCode, 6, Lanes, 64);
    Require(!Faulted(classes), "image table: valid entries faulted:\n" + classes.log);
    for (std::uint32_t group = 0; group < 6; ++group) {
        const auto* words = Table.data() + group * (Stride / 4u);
        const auto control = RunDirect(device, {words[0], words[1], words[2], words[3]}, {words[4], words[5], words[6], words[7]});
        Require(Group(classes, group) == Group(control, 0), "image table: record " + std::to_string(group) + " differs from the direct binding of the same words");
    }
    RequireRed(classes, 0, Lanes, [](std::uint32_t x) { return std::bit_cast<std::uint32_t>(FloatTexels[x]); }, "float texels");
    RequireRed(classes, 1, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "uint texels");
    RequireRed(classes, 2, Lanes, [](std::uint32_t x) { return static_cast<std::uint32_t>(SintTexels[x]); }, "sint texels");
    RequireRed(classes, 4, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "uint texels through a linear sampler");

    Keys[0] = 6;
    Keys[1] = 1000;
    Keys[2] = 0x10000000u;
    const auto zeros = Run(device, TableCode, 3, Lanes, 64);
    Require(!Faulted(zeros), "image table: a null entry or an out-of-range key faulted:\n" + zeros.log);
    for (std::size_t index = 0; index < 2 * Lanes * 4; ++index) {
        Require(zeros.words[index] == 0u, "image table: a null entry or an out-of-range key did not sample zeros (word " + std::to_string(index) + " is " + Hex(zeros.words[index]) + ")");
    }
    Require(Group(zeros, 2) == Group(classes, 0), "image table: a key that wraps onto record 0 did not sample record 0");

    Keys[0] = 0x05555556u;
    const auto wrapped = Run(device, TableCode, 1, Lanes, 64);
    Require(Faulted(wrapped) && wrapped.log.find("outside the snapshot") != std::string::npos, "image table: a mid-record key did not fault as outside the snapshot:\n" + wrapped.log);

    Keys[0] = 0;
    const auto unused = Run(device, TableCode, 1, Lanes, 64);
    Require(!Faulted(unused) && Group(unused, 0) == Group(classes, 0), "image table: an unreferenced invalid entry changed the result");
    Keys[0] = 7;
    const auto cut = Run(device, TableCutCode, 1, Lanes, 64);
    Require(!Faulted(cut), "image table: an invalid entry selected with EXEC clear faulted:\n" + cut.log);
    const auto selected = Run(device, TableCode, 1, Lanes, 64);
    const auto tableAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Table.data())) + 7u * Stride + 16u;
    char address[32];
    std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(tableAddress));
    Require(Faulted(selected) && selected.log.find("not an image") != std::string::npos && selected.log.find(address) != std::string::npos && selected.log.find("00001234") != std::string::npos, "image table: an invalid entry selected with EXEC set did not report its address, words and reason:\n" + selected.log);
    Require(std::all_of(selected.words.begin(), selected.words.end(), [](std::uint32_t word) { return word == 0u || word == 0xdeadbeefu; }), "image table: a faulting access did not sample zeros");

    void* reserved = nullptr;
#ifdef _WIN32
    reserved = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
#else
    reserved = mmap(nullptr, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserved == MAP_FAILED) reserved = nullptr;
#endif
    Require(reserved != nullptr, "cannot reserve an inaccessible range");
    const auto unmappedImage = Image(reserved, Format32Float);
    SetRecord(8, PointClamp, unmappedImage);
    Keys[0] = 0;
    const auto unmappedUnused = Run(device, TableCode, 1, Lanes, 64);
    Require(!Faulted(unmappedUnused) && Group(unmappedUnused, 0) == Group(classes, 0), "image table: an unreferenced unmapped entry changed the result:\n" + unmappedUnused.log);
    Keys[0] = 8;
    const auto unmappedUsed = Run(device, TableCode, 1, Lanes, 64);
    const auto unmappedControl = RunDirect(device, PointClamp, unmappedImage);
    Require(Faulted(unmappedUsed) ? unmappedUsed.log.find("rejected by the driver") != std::string::npos || unmappedUsed.log.find("unmapped") != std::string::npos : Group(unmappedUsed, 0) == Group(unmappedControl, 0), "image table: an unmapped entry neither faulted nor matched the direct binding:\n" + unmappedUsed.log);
    SetRecord(8, PointClamp, floatImage);
#ifdef _WIN32
    VirtualFree(reserved, 0, MEM_RELEASE);
#else
    munmap(reserved, 65536);
#endif

    const auto srgbImage = Image(UintTexels.data(), Format8_8Srgb);
    SetRecord(9, PointClamp, srgbImage);
    Keys[0] = 0;
    const auto srgbUnused = Run(device, TableCode, 1, Lanes, 64);
    Require(!Faulted(srgbUnused) && Group(srgbUnused, 0) == Group(classes, 0), "image table: an unreferenced 8_8_SRGB entry changed the result:\n" + srgbUnused.log);
    Keys[0] = 9;
    const auto srgbUsed = Run(device, TableCode, 1, Lanes, 64);
    Require(Faulted(srgbUsed) && srgbUsed.log.find("format conversion") != std::string::npos, "image table: an 8_8_SRGB entry decoded in the shader did not fault as a format conversion:\n" + srgbUsed.log);
    Require(std::all_of(srgbUsed.words.begin(), srgbUsed.words.end(), [](std::uint32_t word) { return word == 0u || word == 0xdeadbeefu; }), "image table: an 8_8_SRGB entry decoded in the shader did not sample zeros");
    std::string refusal;
    try {
        static_cast<void>(RunDirect(device, PointClamp, srgbImage));
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    Require(refusal.find("samples or gathers an sRGB image the device cannot sample") != std::string::npos, "image table: the direct binding of the 8_8_SRGB entry was not refused: " + refusal);
    SetRecord(9, PointClamp, floatImage);

    Keys[0] = 0;
    const auto before = Run(device, TableCode, 1, Lanes, 64);
    SetRecord(0, PointClamp, Image(OtherFloatTexels.data(), Format32Float));
    const auto after = Run(device, TableCode, 1, Lanes, 64);
    Require(after.variantId == before.variantId, "image table: a table with the same classes compiled another variant");
    RequireRed(after, 0, Lanes, [](std::uint32_t x) { return std::bit_cast<std::uint32_t>(OtherFloatTexels[x]); }, "changed table contents");
    OtherFloatTexels[5] = 1234.5f;
    const auto texels = Run(device, TableCode, 1, Lanes, 64);
    RequireRed(texels, 0, Lanes, [](std::uint32_t x) { return std::bit_cast<std::uint32_t>(OtherFloatTexels[x]); }, "changed texels");
    SetRecord(0, PointClamp, floatImage);
    Require(before.imageTableCount == 16u, "image table: a three-entry table was not bound in the smallest bucket");

    Keys[0] = 0;
    bool aliased = false;
    try {
        static_cast<void>(Run(device, TableCode, 1, Lanes, 64, true));
    } catch (const std::exception& error) {
        aliased = std::string(error.what()).find("is written by its own dispatch") != std::string::npos;
    }
    Require(aliased, "image table: a dispatch storing into its own table was recorded");

    std::copy(LinearWrap.begin(), LinearWrap.end(), Srt.begin() + 12);
    Keys[0] = 2;
    Keys[1] = 0;
    Keys[2] = 1;
    const auto integerLinear = Run(device, TableDirectSamplerCode, 3, Lanes, 64);
    Require(!Faulted(integerLinear), "image table: a direct sampler with a table T# faulted:\n" + integerLinear.log);
    const auto sintControl = RunDirect(device, LinearWrap, sintImage);
    const auto floatControl = RunDirect(device, LinearWrap, floatImage);
    const auto uintControl = RunDirect(device, LinearWrap, uintImage);
    Require(floatControl.samplers == std::vector{LinearWrap} && uintControl.samplers == std::vector{PointWrap} && sintControl.samplers == std::vector{PointWrap}, "image table: a direct binding under a linear S# is not point-filtered exactly for its integer images");
    Require(Group(integerLinear, 0) == Group(sintControl, 0) && Group(integerLinear, 1) == Group(floatControl, 0) && Group(integerLinear, 2) == Group(uintControl, 0), "image table: an entry under a linear direct S# differs from the direct binding");
    RequireRed(integerLinear, 0, Lanes, [](std::uint32_t x) { return static_cast<std::uint32_t>(SintTexels[x]); }, "sint texels through a linear direct sampler");
    RequireRed(integerLinear, 2, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "uint texels through a linear direct sampler");
    RequireRed(uintControl, 0, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "direct uint texels through a linear sampler");

    Keys = {};
    Keys[0] = 1;
    const auto uintLinear = Run(device, TableDirectSamplerCode, 2, Lanes, 64);
    Require(!Faulted(uintLinear), "image table: a direct sampler with float and uint table T#s faulted:\n" + uintLinear.log);
    Require(Holds(uintLinear.samplers, LinearWrap) && Holds(uintLinear.samplers, PointWrap), "image table: a linear direct S# with float and uint table T#s has no point-filtered copy");
    Require(Group(uintLinear, 0) == Group(uintControl, 0) && Group(uintLinear, 1) == Group(floatControl, 0), "image table: a uint or float entry under a linear direct S# differs from the direct binding");

    Keys[0] = 4;
    const auto uintTable = Run(device, TableCode, 1, Lanes, 64);
    Require(!Faulted(uintTable), "image table: a table S# with float and uint table T#s faulted:\n" + uintTable.log);
    Require(TablePointCopy(uintTable, LinearWrap, PointWrap), "image table: a linear table S# with float and uint table T#s has no point-filtered copy");
    Require(Group(uintTable, 0) == Group(uintControl, 0), "image table: a uint entry under a linear table S# differs from the direct binding");

    std::copy(uintImage.begin(), uintImage.end(), Srt.begin() + 16);
    Keys[0] = 4;
    Keys[1] = 1;
    const auto tableSampler = Run(device, TableSamplerCode, 2, Lanes, 64);
    Require(!Faulted(tableSampler), "image table: a table sampler with a direct T# faulted:\n" + tableSampler.log);
    Require(TablePointCopy(tableSampler, LinearWrap, PointWrap), "image table: a linear table S# with a direct uint T# has no point-filtered copy");
    Require(Group(tableSampler, 0) == Group(uintControl, 0), "image table: a direct uint T# under a linear table S# differs from the direct binding");
    RequireRed(tableSampler, 0, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "direct uint texels through a linear table sampler");
    RequireRed(tableSampler, 1, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "direct uint texels through a point table sampler");

    RunWaveKeys(device, TableWave32Code, "image table");
}

template <std::size_t CodeWords>
Outcome RunReadDirect(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, CodeWords>& code, const std::array<std::uint32_t, 4>& image) {
    std::copy(PointClamp.begin(), PointClamp.end(), Srt.begin() + 12);
    std::copy(image.begin(), image.end(), Srt.begin() + 16);
    return Run(device, code, 1, Lanes, 64);
}

void RunReadTests(AgcDriver::VulkanDevice& device) {
    const auto floatImage = Image(FloatTexels.data(), Format32Float);
    const auto uintImage = Image(UintTexels.data(), Format32UInt);
    const auto sintImage = Image(SintTexels.data(), Format32SInt);
    SetRecord(0, PointClamp, floatImage);
    SetRecord(1, PointClamp, uintImage);
    SetRecord(2, PointClamp, sintImage);
    SetRecord(3, LinearWrap, floatImage);
    SetRecord(6, {0u, 0u, 0u, 0u}, {0u, 0u, 0u, 0u});
    SetRecord(7, PointClamp, {0x1234u, 0x5678u, 0x9abcu, 0x0000ffacu});
    Keys = {};
    const std::array<std::uint32_t, 5> keys{0u, 1u, 2u, 3u, 6u};
    std::copy(keys.begin(), keys.end(), Keys.begin());
    const auto reads = Run(device, ReadTableCode, 5, Lanes, 64);
    Require(!Faulted(reads), "image table read: valid entries faulted:\n" + reads.log);
    for (std::uint32_t group = 0; group < 4; ++group) {
        const auto* words = Table.data() + keys[group] * (Stride / 4u);
        const auto control = RunReadDirect(device, ReadDirectCode, {words[4], words[5], words[6], words[7]});
        Require(Group(reads, group) == Group(control, 0), "image table read: record " + std::to_string(keys[group]) + " differs from the direct binding of the same words");
    }
    RequireRed(reads, 0, Lanes, [](std::uint32_t x) { return std::bit_cast<std::uint32_t>(FloatTexels[x]); }, "image table read: float texels");
    RequireRed(reads, 1, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "image table read: uint texels");
    RequireRed(reads, 2, Lanes, [](std::uint32_t x) { return static_cast<std::uint32_t>(SintTexels[x]); }, "image table read: sint texels");
    const auto nullGroup = Group(reads, 4);
    Require(std::all_of(nullGroup.begin(), nullGroup.end(), [](std::uint32_t word) { return word == 0u; }), "image table read: a null entry did not read zeros");

    Keys[0] = 7;
    const auto cut = Run(device, ReadTableCutCode, 1, Lanes, 64);
    Require(!Faulted(cut), "image table read: an invalid entry read with EXEC clear faulted:\n" + cut.log);
    const auto selected = Run(device, ReadTableCode, 1, Lanes, 64);
    Require(Faulted(selected) && selected.log.find("not an image") != std::string::npos && selected.log.find("00001234") != std::string::npos, "image table read: an invalid entry read with EXEC set did not report its words and reason:\n" + selected.log);

    Keys = {};
    Keys[1] = 1u;
    const auto halves = Run(device, ReadTableD16Code, 2, Lanes, 64);
    Require(!Faulted(halves), "image table d16 read: valid entries faulted:\n" + halves.log);
    for (std::uint32_t group = 0; group < 2; ++group) {
        const auto* words = Table.data() + group * (Stride / 4u);
        const auto control = RunReadDirect(device, ReadDirectD16Code, {words[4], words[5], words[6], words[7]});
        Require(Group(halves, group) == Group(control, 0), "image table d16 read: record " + std::to_string(group) + " differs from the direct binding of the same words");
    }
    RequireRed(halves, 0, Lanes, [](std::uint32_t x) { return Half(FloatTexels[x]); }, "image table d16 read: float texels");
    RequireRed(halves, 1, Lanes, [](std::uint32_t x) { return UintTexels[x] & 0xffffu; }, "image table d16 read: uint texels");

    RunWaveKeys(device, ReadTableWave32Code, "image table read");

    std::fill(Root.begin(), Root.end(), 0u);
    SetEntry(0, floatImage);
    SetEntry(1, uintImage);
    SetEntry(2, sintImage);
    SetEntry(255, uintImage);
    const std::array<std::uint32_t, 5> pointerKeys{0u, 1u, 2u, 3u, 0x1ffu};
    Keys = {};
    std::copy(pointerKeys.begin(), pointerKeys.end(), Keys.begin());
    const auto pointer = RunPointer(device, ReadPointerCode, 5);
    Require(!Faulted(pointer), "pointer image table read: valid entries faulted:\n" + pointer.log);
    for (std::uint32_t group = 0; group < pointerKeys.size(); ++group) {
        const auto* words = Root.data() + (PaletteOffset + (pointerKeys[group] & 0xffu) * 32u) / 4u;
        if (std::all_of(words, words + 8, [](std::uint32_t word) { return word == 0u; })) {
            const auto zeros = Group(pointer, group);
            Require(std::all_of(zeros.begin(), zeros.end(), [](std::uint32_t word) { return word == 0u; }), "pointer image table read: a null entry did not read zeros");
            continue;
        }
        const auto control = RunReadDirect(device, ReadDirectCode, {words[0], words[1], words[2], words[3]});
        Require(Group(pointer, group) == Group(control, 0), "pointer image table read: key " + Hex(pointerKeys[group]) + " differs from the direct binding of its masked entry");
    }
    RequireRed(pointer, 4, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "pointer image table read: key 0x1ff masked to entry 255");
}

void RunPointerTests(AgcDriver::VulkanDevice& device) {
    const auto floatImage = Image(FloatTexels.data(), Format32Float);
    const auto uintImage = Image(UintTexels.data(), Format32UInt);
    const auto sintImage = Image(SintTexels.data(), Format32SInt);
    const std::array<std::uint32_t, 4> invalid{0x1234u, 0x5678u, 0x9abcu, 0x0000ffacu};
    std::fill(Root.begin(), Root.end(), 0u);
    SetEntry(0, floatImage);
    SetEntry(1, uintImage);
    SetEntry(2, sintImage);
    SetEntry(7, invalid);
    SetEntry(255, uintImage);
    const std::array<std::uint32_t, 6> keys{0u, 1u, 2u, 3u, 0x1ffu, 0x102u};
    Keys = {};
    std::copy(keys.begin(), keys.end(), Keys.begin());
    const auto palette = RunPointer(device, PointerCode, 6);
    Require(!Faulted(palette), "pointer image table: valid entries faulted:\n" + palette.log);
    for (std::uint32_t group = 0; group < keys.size(); ++group) {
        const auto* words = Root.data() + (PaletteOffset + (keys[group] & 0xffu) * 32u) / 4u;
        if (std::all_of(words, words + 8, [](std::uint32_t word) { return word == 0u; })) {
            const auto zeros = Group(palette, group);
            Require(std::all_of(zeros.begin(), zeros.end(), [](std::uint32_t word) { return word == 0u; }), "pointer image table: a null entry did not sample zeros");
            continue;
        }
        const auto control = RunDirect(device, PointClamp, {words[0], words[1], words[2], words[3]});
        Require(Group(palette, group) == Group(control, 0), "pointer image table: key " + Hex(keys[group]) + " differs from the direct binding of its masked entry");
    }
    RequireRed(palette, 0, Lanes, [](std::uint32_t x) { return std::bit_cast<std::uint32_t>(FloatTexels[x]); }, "pointer image table float texels");
    RequireRed(palette, 2, Lanes, [](std::uint32_t x) { return static_cast<std::uint32_t>(SintTexels[x]); }, "pointer image table sint texels");
    RequireRed(palette, 4, Lanes, [](std::uint32_t x) { return UintTexels[x]; }, "pointer image table key 0x1ff masked to entry 255");

    const auto loaded = RunPointer(device, LoadedPointerCode, 6);
    Require(!Faulted(loaded) && loaded.words == palette.words, "pointer image table: a base loaded from the SRT with a negative immediate differs:\n" + loaded.log);

    Keys[0] = 7;
    const auto selected = RunPointer(device, PointerCode, 1);
    char address[32];
    std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(Root.data()) + PaletteOffset + 7u * 32u));
    Require(Faulted(selected) && selected.log.find("not an image") != std::string::npos && selected.log.find(address) != std::string::npos && selected.log.find("00001234") != std::string::npos, "pointer image table: an invalid entry did not report its address, words and reason:\n" + selected.log);
    Require(std::all_of(selected.words.begin(), selected.words.end(), [](std::uint32_t word) { return word == 0u || word == 0xdeadbeefu; }), "pointer image table: a faulting access did not sample zeros");

    Keys[0] = 2;
    Keys[1] = 0x08000000u;
    const auto unbounded = RunPointer(device, UnboundedPointerCode, 2);
    Require(!Faulted(unbounded) && Group(unbounded, 0) == Group(palette, 2) && Group(unbounded, 1) == Group(palette, 0), "pointer image table: an unmasked key, or one whose offset wraps to entry 0, sampled the wrong entry:\n" + unbounded.log);
    Keys[0] = 300;
    const auto past = RunPointer(device, UnboundedPointerCode, 1);
    Require(Faulted(past) && past.log.find("unmapped") != std::string::npos, "pointer image table: an entry past the captured palette did not fault as unmapped:\n" + past.log);
    Keys[0] = 0x10000u;
    const auto capped = RunPointer(device, UnboundedPointerCode, 1);
    Require(Faulted(capped) && capped.log.find("outside the snapshot") != std::string::npos, "pointer image table: a key past the key cap did not fault as outside the snapshot:\n" + capped.log);
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_SRGB_SHADER_DECODE", "1");
#else
        setenv("APS5_SRGB_SHADER_DECODE", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        RunTests(*device);
        RunPointerTests(*device);
        RunReadTests(*device);
        std::puts("image table tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

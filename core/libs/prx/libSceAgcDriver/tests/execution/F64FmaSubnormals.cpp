#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 8;
constexpr std::uint32_t Results = 2;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 12> Code{
    0x34020085, 0x34060083, 0xe0381000, 0x80000401, 0xe0381010, 0x80000801, 0xbf8c3f70, 0xd54c000c,
    0x04220d04, 0xe0741000, 0x80010c03, 0xbf810000,
};

constexpr std::uint64_t Rows[Threads][4] = {
    {0xaf909f1570e40fb2ull, 0x10697a78dd1df49bull, 0x8000000000000000ull, 0x800d3be6095b3463ull},
    {0x1e50511fd30f5d67ull, 0x24c92a1b12df5283ull, 0x8329a9b24976c10bull, 0x0006cc7690f01937ull},
    {0x98cd9ffcf3ed94edull, 0x2a51579178c2f7a9ull, 0x03300e10060c0a21ull, 0x000826b2393bc091ull},
    {0x2c67bd24a078a9e3ull, 0x169c3293f5b93146ull, 0x8314eb0558ce14d3ull, 0x8002b781181390c7ull},
    {0xbcca08e86f6c243dull, 0x0647cf3a0cd21b23ull, 0x03235effea3a0f40ull, 0x00053433dc65a9cbull},
    {0x97cb9cf8c8a2fbb6ull, 0xab2cb31ef6264b43ull, 0x8308c3ea76c1ebe5ull, 0x00014c7d2a0da8cbull},
    {0x0946591c0410303cull, 0xba059dbfbeb6d4adull, 0x80099e28c52b6c3dull, 0x835e314afa5d1a07ull},
    {0xcee37832c306ee31ull, 0x80017e95cd7d98dfull, 0x8c168438ac98afa4ull, 0x0ecd18c0555dceefull},
    {0x800e8b91a326e01dull, 0xd020fb9f53a2df2cull, 0x8cb514f5ff6d74b0ull, 0x103ee09fedab559aull},
    {0x401fac71cda29715ull, 0x8008000020000000ull, 0x002fac724c545e4full, 0x0000000000000007ull},
    {0x2e4c000000000000ull, 0x11aeb6db849700ffull, 0x800d70000a021070ull, 0x8000000000000000ull},
    {0x41d0000000000008ull, 0x0000000040000002ull, 0x80761e2b2ef2fce7ull, 0x0084f0ea6986819dull},
    {0xfc21916e447cf802ull, 0x8004eec859174483ull, 0x3be2fe907aa80074ull, 0x3c26da145be5eaacull},
    {0x35d175fd70782c6cull, 0x0c6033e7239c8fe0ull, 0x0005cb65548dabd1ull, 0x0241aea1c6a7627dull},
    {0xb4fb1dc3f1ce6211ull, 0x8b80000000000100ull, 0x800000000b9b5f36ull, 0x008b1dc3f1b72d04ull},
    {0x0008000000000000ull, 0x8004000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {0x0008000000000000ull, 0x0004000000000000ull, 0x0170000000000000ull, 0x0170000000000000ull},
    {0x4004000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000002ull},
    {0x3fe0000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {0x3fe0000000000001ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000001ull},
    {0x3fefffffffffffffull, 0x0010000000000000ull, 0x0000000000000000ull, 0x0010000000000000ull},
    {0x1e60000000000000ull, 0x1e60000000000000ull, 0x8000000000000001ull, 0x0000000000000000ull},
    {0x3ff0000000000001ull, 0x0018000000000001ull, 0x8010000000000000ull, 0x0008000000000003ull},
    {0x3ff8000000000000ull, 0x4000000000000000ull, 0x3fd0000000000000ull, 0x400a000000000000ull},
    {0x3ff0000000000001ull, 0x3ff0000000000001ull, 0xbff0000000000000ull, 0x3cc0000000000000ull},
    {0xffefffffffffffffull, 0x3ff0000000000000ull, 0x000fffffffffffffull, 0xffefffffffffffffull},
    {0x5fefffffffffffffull, 0x5fefffffffffffffull, 0x0000000000000000ull, 0x7feffffffffffffeull},
    {0x000fffffffffffffull, 0x4000000000000000ull, 0x8000000000000001ull, 0x001ffffffffffffdull},
    {0x8000000000000003ull, 0x7fe8000000000000ull, 0x3ff0000000000000ull, 0x3fefffffffffffeeull},
    {0x1a75555555555555ull, 0x000aaaaaaaaaaaabull, 0x0000000000000000ull, 0x0000000000000000ull},
    {0x0173000000000000ull, 0x3e17000000000000ull, 0x80001b5000000000ull, 0x0000000000000000ull},
    {0xa000000000000001ull, 0x1fffffffffffffffull, 0x0010000000000000ull, 0x8000000000000000ull},
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

void Run(AgcDriver::VulkanDevice& device) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t operand = 0; operand < 3; ++operand) {
            Input[tid * Inputs + operand * 2] = static_cast<std::uint32_t>(Rows[tid][operand]);
            Input[tid * Inputs + operand * 2 + 1] = static_cast<std::uint32_t>(Rows[tid][operand] >> 32u);
        }
    }
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check() {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint64_t actual = Output[tid * Results] | (static_cast<std::uint64_t>(Output[tid * Results + 1]) << 32u);
        Require(actual == Rows[tid][3], "v_fma_f64: lane " + std::to_string(tid) + " fma(" + Hex(Rows[tid][0]) + ", " + Hex(Rows[tid][1]) + ", " + Hex(Rows[tid][2]) + ") is " + Hex(actual) + ", expected " + Hex(Rows[tid][3]));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        Check();
        std::puts("v_fma_f64 subnormal tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

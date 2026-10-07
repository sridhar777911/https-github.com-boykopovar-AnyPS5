#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {

struct Counts {
    std::size_t unsignedExtracts = 0;
    std::size_t halfUnpacks = 0;
};

Counts count(const std::vector<std::uint32_t>& words) {
    Counts counts;
    for (std::size_t at = 5; at < words.size();) {
        const auto length = words[at] >> 16u;
        const auto opcode = words[at] & 0xffffu;
        if (length == 0 || at + length > words.size()) break;
        if (opcode == 203u) ++counts.unsignedExtracts;
        if (opcode == 12u && length >= 5 && words[at + 4] == 62u) ++counts.halfUnpacks;
        at += length;
    }
    return counts;
}

Counts recompile(std::uint8_t mode) {
    using namespace ShaderRecompiler;
    static const std::array<std::uint32_t, 3> code{0xf8001c0fu, 0x100u, 0xbf810000u};
    ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 0;
    pixel.inputAddr = 0x2;
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.targetOutputMode[0] = mode;
    pixel.targetExportMapping.fill(0xe4u);
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return count(Recompile(request).spirv);
}

}

int main() {
    try {
        const auto uint16 = recompile(7);
        if (uint16.unsignedExtracts != 4 || uint16.halfUnpacks != 0) {
            std::fprintf(stderr, "UINT16_ABGR export: %zu unsigned extracts, %zu half unpacks\n", uint16.unsignedExtracts, uint16.halfUnpacks);
            return 1;
        }
        const auto half = recompile(4);
        if (half.unsignedExtracts != 0 || half.halfUnpacks != 2) {
            std::fprintf(stderr, "FP16_ABGR export: %zu unsigned extracts, %zu half unpacks\n", half.unsignedExtracts, half.halfUnpacks);
            return 1;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected error: %s\n", error.what());
        return 1;
    }
    return 0;
}

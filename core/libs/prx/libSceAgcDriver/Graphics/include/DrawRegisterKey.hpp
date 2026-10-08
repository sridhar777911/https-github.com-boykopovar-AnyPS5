#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWREGISTERKEY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWREGISTERKEY_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <algorithm>
#include <cstdint>

namespace AgcDriver::Graphics {

enum class RegisterBank : std::uint8_t { Context, Shader, UserConfig, Count };
struct RegisterRead {
    RegisterBank bank;
    std::uint32_t offset;
};

// The frozen table of the registers the draw decoders read, as [first, first + count) per bank in
// bank and offset order: the draw key (Driver::draw) hashes every present register of these
// ranges with its offset, so the decoded state, pixel and program inputs are a pure function of
// the key. A range covers whole register groups (the eight CB_COLOR slots, the 32 user words of
// each bank, the interpolator controls) whether or not a draw's slot count or user count reaches
// them: a stricter key, never a wrong one. A register the decoders read that the table lacks would
// be a wrong hit, which the facade's log finds (DrawKeyCovers) on every verified miss.
struct DrawKeyRange {
    RegisterBank bank;
    std::uint32_t first;
    std::uint32_t count;
};
inline constexpr std::array<DrawKeyRange, 45> DrawKeyRegisters{{
    {RegisterBank::Context, 0x000, 1}, {RegisterBank::Context, 0x002, 1}, {RegisterBank::Context, 0x007, 7}, {RegisterBank::Context, 0x010, 6}, {RegisterBank::Context, 0x01a, 4},
    {RegisterBank::Context, 0x080, 4}, {RegisterBank::Context, 0x08c, 4}, {RegisterBank::Context, 0x090, 2}, {RegisterBank::Context, 0x094, 2}, {RegisterBank::Context, 0x0b4, 2}, {RegisterBank::Context, 0x105, 4}, {RegisterBank::Context, 0x10b, 3}, {RegisterBank::Context, 0x10f, 6},
    // SPI_PS_INPUT_CNTL_0..31, SPI_PS_INPUT_ENA/ADDR, SPI_PS_IN_CONTROL, SPI_SHADER_POS/Z/COL_FORMAT,
    // CB_BLEND0..7_CONTROL, GE_MAX_OUTPUT_PER_SUBGROUP.
    {RegisterBank::Context, 0x191, 32}, {RegisterBank::Context, 0x1b3, 2}, {RegisterBank::Context, 0x1b6, 1}, {RegisterBank::Context, 0x1c3, 3}, {RegisterBank::Context, 0x1e0, 8}, {RegisterBank::Context, 0x1ff, 1},
    // DB_DEPTH_CONTROL .. PA_CL_VS_OUT_CNTL, PA_SC_MODE_CNTL_0/1, VGT_GS_MODE, VGT_GS_VERT_ITEMSIZE,
    // PA_SU_VTX_CNTL, the sample masks, PA_SC_CONSERVATIVE_RASTERIZATION_CNTL.
    {RegisterBank::Context, 0x200, 8}, {RegisterBank::Context, 0x292, 2}, {RegisterBank::Context, 0x29b, 1}, {RegisterBank::Context, 0x2ab, 1}, {RegisterBank::Context, 0x2ce, 1}, {RegisterBank::Context, 0x2d5, 2}, {RegisterBank::Context, 0x2db, 2}, {RegisterBank::Context, 0x2de, 6}, {RegisterBank::Context, 0x2f8, 2}, {RegisterBank::Context, 0x30e, 2}, {RegisterBank::Context, 0x313, 1},
    // CB_COLOR0..7_BASE .. DCC_BASE (15 words a slot), CB_COLOR0..7_BASE_EXT, DCC_BASE_EXT, ATTRIB2, ATTRIB3.
    {RegisterBank::Context, 0x318, 0x78}, {RegisterBank::Context, 0x390, 8}, {RegisterBank::Context, 0x3a8, 0x18},
    // The pixel program address, RSRC2 and user words; the geometry-back user pointer and program
    // address; the vertex/geometry-front RSRC1/RSRC2 and user words; the vertex program address;
    // the hull user pointer, program address, RSRC2 and user words; the local program address.
    {RegisterBank::Shader, 0x008, 0x24}, {RegisterBank::Shader, 0x082, 2}, {RegisterBank::Shader, 0x088, 2}, {RegisterBank::Shader, 0x08a, 0x22}, {RegisterBank::Shader, 0x0c8, 2}, {RegisterBank::Shader, 0x102, 2}, {RegisterBank::Shader, 0x108, 2}, {RegisterBank::Shader, 0x10a, 0x22}, {RegisterBank::Shader, 0x148, 2},
    // GE_PRIM_TYPE, GE_MULTI_PRIM_IB_RESET_EN, the geometry subgroup sizes.
    {RegisterBank::UserConfig, 0x242, 1}, {RegisterBank::UserConfig, 0x24b, 1}, {RegisterBank::UserConfig, 0x25b, 1},
}};

inline std::uint64_t DrawRegisterKey(const QueueState& queue, std::uint64_t deviceSerial, bool allUserWords = false) {
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    const auto userEnd = [&](std::uint32_t base) {
        if (allUserWords) return base + 32u;
        const auto resources = queue.shader.find(base - 1);
        if (resources == queue.shader.end()) return base;
        const auto count = ((resources->second >> 1u) & 0x1fu) | (((resources->second >> 27u) & 1u) << 5u);
        return base + std::min(count, 32u);
    };
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> users{{{0x00cu, userEnd(0x00cu)}, {0x08cu, userEnd(0x08cu)}, {0x10cu, userEnd(0x10cu)}}};
    const auto unread = [&](std::uint32_t offset) {
        return std::any_of(users.begin(), users.end(), [&](const auto& user) { return offset >= user.second && offset < user.first + 32u; });
    };
    mix(deviceSerial);
    for (const auto& range : DrawKeyRegisters) {
        const auto& bank = range.bank == RegisterBank::Context ? queue.context : range.bank == RegisterBank::Shader ? queue.shader : queue.userConfig;
        mix((static_cast<std::uint64_t>(range.bank) << 32u) | range.first);
        const auto end = range.first + range.count;
        const bool shader = range.bank == RegisterBank::Shader;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            if (shader && unread(it->first)) continue;
            mix(it->first);
            mix(it->second);
        }
    }
    return key;
}

}

#endif

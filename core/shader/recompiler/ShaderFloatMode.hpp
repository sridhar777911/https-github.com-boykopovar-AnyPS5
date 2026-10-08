#ifndef CORE_SHADER_RECOMPILER_SHADERFLOATMODE_HPP
#define CORE_SHADER_RECOMPILER_SHADERFLOATMODE_HPP

#include <cstdint>

namespace ShaderRecompiler {

struct ShaderFloatMode {
    std::uint8_t floatMode;
    bool dx10Clamp;
    bool ieee;
    bool fp16Overflow;

    static constexpr ShaderFloatMode Decode(std::uint32_t rsrc1) {
        return {static_cast<std::uint8_t>(rsrc1 >> 12u), (rsrc1 & (1u << 21u)) != 0,
                (rsrc1 & (1u << 23u)) != 0, (rsrc1 & (1u << 26u)) != 0};
    }

    constexpr bool operator==(const ShaderFloatMode&) const = default;
};

}

#endif

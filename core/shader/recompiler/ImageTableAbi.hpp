#ifndef CORE_SHADER_RECOMPILER_IMAGETABLEABI_HPP
#define CORE_SHADER_RECOMPILER_IMAGETABLEABI_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace ShaderRecompiler::ImageTableAbi {

inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint32_t HeaderWords = 4;
inline constexpr std::uint32_t ViewWords = 8;
inline constexpr std::uint32_t ViewMapStart = 0;
inline constexpr std::uint32_t ViewKeyCount = 1;
inline constexpr std::uint32_t ViewSizeLow = 2;
inline constexpr std::uint32_t ViewSizeHigh = 3;
inline constexpr std::uint32_t ViewBaseLow = 4;
inline constexpr std::uint32_t ViewBaseHigh = 5;
inline constexpr std::uint32_t ViewZeroCode = 6;

inline constexpr std::uint32_t NullCode = 0;
inline constexpr std::uint32_t PoisonFlag = 0x80000000u;
inline constexpr std::uint32_t SamplerFlag = 0x40000000u;
inline constexpr std::uint32_t ClassShift = 28;
inline constexpr std::uint32_t ClassMask = 3;
inline constexpr std::uint32_t ElementMask = 0x00ffffffu;
inline constexpr std::uint32_t FloatClass = 1;
inline constexpr std::uint32_t UintClass = 2;
inline constexpr std::uint32_t SintClass = 3;

inline constexpr std::uint32_t FloatBit = 1u << 0u;
inline constexpr std::uint32_t UintBit = 1u << 1u;
inline constexpr std::uint32_t SintBit = 1u << 2u;
inline constexpr std::uint32_t SamplerUsedBit = 1u << 0u;
inline constexpr std::uint32_t SamplerPointBit = 1u << 1u;

inline constexpr std::uint32_t MaxKeys = 65536;
inline constexpr std::uint32_t ImageBudget = 16384;
inline constexpr std::uint32_t SamplerBudget = 256;
inline constexpr std::uint32_t MaxViews = 64;

enum class PoisonReason : std::uint32_t { OutsideSnapshot, OutsideDomain, NotImage, InvalidFormat, Dimension, Multisampled, Cube, Conversion, DepthBits, Fmask, NumericClass, Unmapped, Budget, Reduction, DriverRejected };

constexpr std::string_view PoisonReasonName(PoisonReason reason) {
    switch (reason) {
        case PoisonReason::OutsideSnapshot: return "outside the snapshot";
        case PoisonReason::OutsideDomain: return "outside the key domain";
        case PoisonReason::NotImage: return "not an image";
        case PoisonReason::InvalidFormat: return "invalid format";
        case PoisonReason::Dimension: return "dimension";
        case PoisonReason::Multisampled: return "multisampled";
        case PoisonReason::Cube: return "cube";
        case PoisonReason::Conversion: return "format conversion";
        case PoisonReason::DepthBits: return "depth bits";
        case PoisonReason::Fmask: return "FMASK";
        case PoisonReason::NumericClass: return "numeric class";
        case PoisonReason::Unmapped: return "unmapped";
        case PoisonReason::Budget: return "over budget";
        case PoisonReason::Reduction: return "min or max reduction";
        case PoisonReason::DriverRejected: return "rejected by the driver";
    }
    return "unknown";
}

constexpr std::uint32_t ImageCode(std::uint32_t numericClass, std::uint32_t element) {
    return (numericClass << ClassShift) | element;
}

constexpr std::uint32_t SamplerCode(std::uint32_t entry) {
    return SamplerFlag | entry;
}

constexpr std::uint32_t PoisonCode(std::uint32_t poison) {
    return PoisonFlag | poison;
}

constexpr std::uint64_t BufferBytes(std::uint32_t stride, std::uint32_t records) {
    return stride != 0u ? static_cast<std::uint64_t>(stride) * records : records;
}

constexpr std::optional<std::uint32_t> ScalarBufferDword(std::uint32_t offset, std::uint32_t immediate, std::uint64_t size) {
    const std::uint64_t address = static_cast<std::uint64_t>(offset) + immediate;
    if (address > 0xffffffffull) return std::nullopt;
    const std::uint32_t dword = static_cast<std::uint32_t>(address) & ~3u;
    if (dword >= size) return std::nullopt;
    return dword;
}

constexpr std::optional<std::uint64_t> ScalarAddressDword(std::uint64_t base, std::uint32_t offset, std::uint32_t immediate) {
    const std::uint64_t first = base + (offset & ~3u);
    if (first < base) return std::nullopt;
    const std::int64_t delta = static_cast<std::int32_t>(immediate & ~3u);
    const std::uint64_t magnitude = delta < 0 ? static_cast<std::uint64_t>(-delta) : static_cast<std::uint64_t>(delta);
    if (delta < 0) return magnitude > first ? std::nullopt : std::optional<std::uint64_t>(first - magnitude);
    return first + magnitude < first ? std::nullopt : std::optional<std::uint64_t>(first + magnitude);
}

constexpr std::uint32_t ScalarAddressRecords(std::uint32_t stride) {
    return stride <= 1u ? 0xffffffffu * stride : 0xffffffffu / stride + 1u;
}

constexpr std::uint32_t KeyRecords(std::uint32_t maxKey, std::uint32_t stride) {
    return maxKey != 0xffffffffu && static_cast<std::uint64_t>(maxKey) * stride <= 0xffffffffull ? maxKey + 1u : 0xffffffffu;
}

constexpr std::uint32_t ScalarBufferRecords(std::uint32_t addend, std::uint32_t stride, std::uint32_t immediate, std::uint64_t size) {
    const std::uint64_t start = static_cast<std::uint64_t>(addend) + immediate;
    const std::uint64_t rounded = (size + 3u) & ~std::uint64_t{3u};
    const std::uint64_t limit = rounded < 0x100000000ull ? rounded : 0x100000000ull;
    if (stride == 0u || start >= limit) return 0u;
    const std::uint64_t records = (limit - start + stride - 1u) / stride;
    return records > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(records);
}

}

#endif

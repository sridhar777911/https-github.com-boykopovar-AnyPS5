#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawRegisterKey.hpp"
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include "Recompiler.hpp"

namespace AgcDriver::Graphics {

enum class ShaderPath {
    Vertex,
    Geometry,
    Tessellation,
    TessellationGeometry
};

struct ShaderStages {
    ShaderPath path;
    std::uint32_t registerValue;
    std::uint32_t vertexWaveSize;
    std::uint32_t fragmentWaveSize;
    std::optional<ShaderRecompiler::MeshConfiguration> mesh;
    std::optional<ShaderRecompiler::TessellationConfiguration> tessellation;
};

struct ColorTarget {
    std::uint64_t address;
    VkExtent2D extent;
    VkFormat format;
    std::size_t bytes;
    std::uint8_t componentMapping;
    ColorTileMode tileMode = ColorTileMode::Linear;
    std::uint32_t elementBytes = 4;
    // DCC metadata of a compressed target (CB_COLOR_INFO DCC_ENABLE), or 0 (see DccMetadata.hpp).
    std::uint64_t dccAddress = 0;
    bool dccAlphaOnMsb = false;
    std::uint64_t cmaskAddress = 0;
    std::size_t cmaskBytes = 0;
    std::uint64_t surfaceAddress = 0;
    VkExtent2D surfaceExtent{};
    std::uint32_t mipCount = 1;
    std::uint32_t mip = 0;
    bool mipTail = false;
    std::array<std::uint32_t, 2> clearWords{};
    std::uint32_t slot = 0;
    std::uint32_t depth = 1;
    std::uint32_t depthSlice = 0;
    std::uint32_t exportIndex = 0;
};

struct DepthTarget {
    std::uint64_t address;
    std::uint64_t stencilAddress;
    VkExtent2D extent;
    VkFormat format;
    float clearDepth;
    std::uint8_t clearStencil;
};

struct State {
    ShaderStages stages;
    std::optional<DepthTarget> depth;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    bool depthBoundsTest = false;
    float minDepthBounds = 0.0f;
    float maxDepthBounds = 1.0f;
    bool depthBias = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
    bool stencilTest = false;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};
    ColorTarget color;
    std::vector<ColorTarget> colors;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    bool hasColorTarget;
    bool rectList = false;
    VkExtent2D renderExtent;
    VkPrimitiveTopology topology;
    bool primitiveRestart = false;
    VkViewport viewport;
    bool negativeOneToOne;
    bool depthClamp = false;
    VkRect2D scissor;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    VkPipelineColorBlendAttachmentState blend;
    std::array<float, 4> blendConstants;
};

ShaderStages DecodeShaderStages(const QueueState& queue);
State DecodeState(const QueueState& queue);
std::array<std::uint8_t, 8> ExportMappings(const State& state);
ColorTarget DecodeColorBuffer(const Registers& context, std::uint32_t slot);
std::size_t CmaskBytes(std::uint32_t width, std::uint32_t height);

struct ColorMetadataPass {
    enum class Mode { EliminateFastClear, DccDecompress };
    Mode mode;
    std::vector<ColorTarget> targets;
};
std::optional<ColorMetadataPass> DecodeColorMetadataPass(const QueueState& queue);
// The message DecodeState (or the pixel stage decode after it) would throw for the register rules
// this precheck covers, evaluated without exceptions before the draw is decoded; empty when they
// pass (DecodeState still checks everything). A register a rule needs that is absent is no verdict.
std::string DrawRejection(const QueueState& queue, bool indexed);
bool PixelProgramUnset(const QueueState& queue);
std::string NullPixelProgramRejection(const QueueState& queue);

// The recording facade of the draw decoders (design_cpu_final M8, step 8a): every register read
// of DecodeShaderStages, DecodeState, DrawRejection, DecodePixelStageInfo and Driver::draw's
// program prepare goes through NoteRegisterRead, which appends the bank and offset to the calling
// thread's log while one is set (null in production; Driver::draw sets one on a miss under
// APS5_VERIFY_DRAW_RECIPE=1 and checks the log against DrawKeyRegisters).
std::vector<RegisterRead>*& RegisterReadLog();
inline void NoteRegisterRead(RegisterBank bank, std::uint32_t offset) {
    if (auto* log = RegisterReadLog()) log->push_back({bank, offset});
}
inline const char* RegisterBankName(RegisterBank bank) {
    return bank == RegisterBank::Context ? "context" : bank == RegisterBank::Shader ? "shader" : "user-config";
}

// Whether DrawKeyRegisters holds the read.
bool DrawKeyCovers(RegisterRead read);

}

#endif

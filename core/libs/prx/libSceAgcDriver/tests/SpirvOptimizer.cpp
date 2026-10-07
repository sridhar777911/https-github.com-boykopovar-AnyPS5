#include "SpirvBackend/SpirvOptimizer.hpp"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;
constexpr std::uint32_t Vulkan11 = 0x00401000u;
constexpr std::uint32_t Spirv13 = 0x00010300u;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message.c_str());
        ++failures;
    }
}

struct NarrowType {
    const char* name;
    unsigned width;
    bool signedInteger;
    bool floating;
    const char* arithmeticCapability;
    const char* unrelatedCapability;
};

constexpr std::array Types{
    NarrowType{"u8", 8, false, false, "Int8", "Int16"},
    NarrowType{"i8", 8, true, false, "Int8", "Int16"},
    NarrowType{"u16", 16, false, false, "Int16", "Int8"},
    NarrowType{"i16", 16, true, false, "Int16", "Int8"},
    NarrowType{"f16", 16, false, true, "Float16", "Int8"},
};

enum class ArithmeticSupport { StorageOnly, Explicit, ImpliedInt8 };

std::string moduleSource(const NarrowType& type, bool vector, ArithmeticSupport support) {
    const auto width = std::to_string(type.width);
    const std::string typeOp = type.floating ? "OpTypeFloat " : "OpTypeInt ";
    const std::string signedness = type.floating ? "" : (type.signedInteger ? " 1" : " 0");
    const std::string conversion = type.floating ? "OpFConvert" : (type.signedInteger ? "OpSConvert" : "OpUConvert");
    const std::string wide = type.floating || type.signedInteger ? "%wide" : "%index";
    std::string text = "OpCapability Shader\nOpCapability ";
    text += type.width == 8 ? "StorageBuffer8BitAccess\n" : "StorageBuffer16BitAccess\n";
    text += "OpCapability " + std::string(type.unrelatedCapability) + "\n";
    if (support == ArithmeticSupport::Explicit) text += "OpCapability " + std::string(type.arithmeticCapability) + "\n";
    if (support == ArithmeticSupport::ImpliedInt8) {
        text += "OpCapability DotProductInput4x8BitKHR\nOpExtension \"SPV_KHR_integer_dot_product\"\n";
    }
    text += type.width == 8 ? "OpExtension \"SPV_KHR_8bit_storage\"\n" : "OpExtension \"SPV_KHR_16bit_storage\"\n";
    text += R"(OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%index = OpTypeInt 32 0
%zero = OpConstant %index 0
)";
    if (wide == "%wide") text += "%wide = " + typeOp + "32" + signedness + "\n";
    text += "%small = " + typeOp + width + signedness + "\n";
    text += "%zeroValue = OpConstant " + wide + " 0\n";
    text += "%value = OpConstant " + wide + " " + std::string(type.floating ? "1.5" : (type.signedInteger ? "-257" : "257")) + "\n";
    if (vector) {
        text += "%wideValue = OpTypeVector " + wide + " 2\n%narrow = OpTypeVector %small 2\n";
        text += "%input = OpConstantComposite %wideValue %zeroValue %value\n";
    }
    const std::string narrow = vector ? "%narrow" : "%small";
    text += "%block = OpTypeStruct " + narrow + "\n";
    text += "%blockPointer = OpTypePointer StorageBuffer %block\n";
    text += "%pointer = OpTypePointer StorageBuffer " + narrow + "\n";
    text += R"(%buffer = OpVariable %blockPointer StorageBuffer
%main = OpFunction %void None %function
%entry = OpLabel
%address = OpAccessChain %pointer %buffer %zero
%dead = OpIAdd %index %zero %zero
)";
    if (vector) {
        text += "%converted = " + conversion + " " + narrow + " %input\n";
        text += "OpStore %address %converted\n";
    } else {
        text += "%convertedZero = " + conversion + " " + narrow + " %zeroValue\n";
        text += "OpStore %address %convertedZero\n";
        text += "%converted = " + conversion + " " + narrow + " %value\n";
        text += "OpStore %address %converted\n";
    }
    return text + "OpReturn\nOpFunctionEnd\n";
}

std::vector<std::uint32_t> assemble(const std::string& source) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        std::fprintf(stderr, "SPIRV-Tools: %s\n", message);
    });
    std::vector<std::uint32_t> words;
    check(tools.Assemble(source, &words), "test shader did not assemble");
    return words;
}

std::size_t opcodeCount(const std::vector<std::uint32_t>& words, spv::Op opcode) {
    std::size_t count = 0;
    for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> spv::WordCountShift) {
        if ((words[offset] & spv::OpCodeMask) == static_cast<std::uint32_t>(opcode)) ++count;
    }
    return count;
}

std::set<std::uint32_t> capabilities(const std::vector<std::uint32_t>& words) {
    std::set<std::uint32_t> result;
    for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> spv::WordCountShift) {
        if ((words[offset] & spv::OpCodeMask) == spv::OpCapability) result.insert(words[offset + 1]);
    }
    return result;
}

void testNarrowConversions(bool optimizationEnabled) {
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    for (const auto& type : Types) {
        for (const bool vector : {false, true}) {
            for (const auto support : {ArithmeticSupport::StorageOnly, ArithmeticSupport::Explicit, ArithmeticSupport::ImpliedInt8}) {
                if (support == ArithmeticSupport::ImpliedInt8 && (type.width != 8 || !vector)) continue;
                const bool arithmetic = support != ArithmeticSupport::StorageOnly;
                const std::string label = std::string(type.name) + (vector ? " vector" : " scalar") +
                    (support == ArithmeticSupport::ImpliedInt8 ? " implied Int8" : (arithmetic ? " arithmetic" : " storage only"));
                const auto input = assemble(moduleSource(type, vector, support));
                if (!validator.Validate(input)) {
                    check(false, label + ": invalid test input");
                    continue;
                }
                try {
                    const auto output = ShaderRecompiler::ValidateAndOptimizeSpirv(input, Vulkan11, Spirv13, false);
                    if (!validator.Validate(output)) {
                        check(false, label + ": optimizer returned invalid SPIR-V");
                        continue;
                    }
                    check(capabilities(output) == capabilities(input), label + ": optimizer changed device capabilities");
                    check(opcodeCount(output, spv::OpStore) == (vector ? 1u : 2u), label + ": optimizer lost buffer stores");
                    if (!optimizationEnabled) {
                        check(output == input, label + ": none mode changed the shader");
                    } else {
                        check(opcodeCount(output, spv::OpIAdd) == 0, label + ": optimizer did not remove dead arithmetic");
                        if (arithmetic && !type.floating) {
                            const auto conversion = type.signedInteger ? spv::OpSConvert : spv::OpUConvert;
                            check(opcodeCount(output, conversion) == 0, label + ": safe constant folding was disabled");
                        }
                    }
                } catch (const std::exception& error) {
                    check(false, label + ": " + error.what());
                }
            }
        }
    }
}

void expectRejected(const std::vector<std::uint32_t>& words, std::uint32_t vulkanVersion, std::uint32_t spirvVersion, const char* reason) {
    try {
        static_cast<void>(ShaderRecompiler::ValidateAndOptimizeSpirv(words, vulkanVersion, spirvVersion, false));
        check(false, std::string("accepted ") + reason);
    } catch (const std::exception&) {
    }
}

void testInvalidInputIsRejected() {
    const auto valid = assemble(moduleSource(Types.front(), false, ArithmeticSupport::StorageOnly));
    expectRejected(valid, 0x00400000u, Spirv13, "SPIR-V 1.3 with Vulkan 1.0");
    expectRejected(valid, Vulkan11, 0x00010200u, "a module newer than the requested SPIR-V version");
    expectRejected(valid, 0u, Spirv13, "an unsupported Vulkan version");
    auto invalid = moduleSource(Types.front(), false, ArithmeticSupport::StorageOnly);
    invalid.insert(invalid.find("%main = OpFunction"), "%illegal = OpConstantNull %small\n");
    expectRejected(assemble(invalid), Vulkan11, Spirv13, "an illegal storage-only narrow constant even when unused");
    auto malformed = valid;
    malformed[5] = spv::OpCapability;
    expectRejected(malformed, Vulkan11, Spirv13, "a zero-length SPIR-V instruction");
}

}

int main() {
    const char* mode = std::getenv("APS5_SPIRV_OPT");
    const bool optimizationEnabled = mode == nullptr || std::string(mode) != "none";
    testNarrowConversions(optimizationEnabled);
    testInvalidInputIsRejected();
    if (failures != 0) {
        std::fprintf(stderr, "%d optimizer check(s) failed\n", failures);
        return 1;
    }
    std::puts("SPIR-V optimizer checks passed");
    return 0;
}

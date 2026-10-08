#include "CacheKey.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace ShaderRecompiler {
bool DebugProbeActive() { return false; }
bool RayTracingStrict() { return false; }
bool RayTracingMiss() { return false; }
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        using namespace ShaderRecompiler;
        const RequestSerializer serializer;
        RecompileRequest request{};
        request.context.waveSize = 32;
        const auto legacy = serializer.Deserialize("NVNQQQkAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAA=");
        require(!legacy.request.context.floatMode && legacy.request.context.waveSize == 32, "a version-9 capture invented a float mode");
        require(!serializer.Deserialize(serializer.Serialize(request)).request.context.floatMode, "an unknown float mode became known");
        require(ShaderFloatMode::Decode(0xffffffffu) == ShaderFloatMode{255, true, true, true}, "RSRC1 fields were decoded incorrectly");
        require(ShaderFloatMode::Decode(0x002c0000u) == ShaderFloatMode{0xc0, true, false, false}, "the common graphics float mode was decoded incorrectly");
        constexpr std::uint32_t mask = 0x00ff000u | (1u << 21u) | (1u << 23u) | (1u << 26u);
        require(ShaderFloatMode::Decode(~mask) == ShaderFloatMode{}, "unrelated RSRC1 bits affected the float mode");
        std::vector<std::uint64_t> unknownKey;
        RecompileCacheKey::Build(request, unknownKey);
        request.context.floatMode = ShaderFloatMode{};
        std::vector<std::uint64_t> key;
        RecompileCacheKey::Build(request, key);
        require(key != unknownKey, "an unknown mode aliases an explicit zero mode");
        std::array<LinkedProgram, 2> linked{};
        linked[0].role = ProgramRole::Main;
        linked[1].role = ProgramRole::GeometryBack;
        linked[0].floatMode = ShaderFloatMode::Decode(0x002c0000u);
        linked[1].floatMode = ShaderFloatMode::Decode(0x04801000u);
        request.graphics = GraphicsCompileContext{0, linked, std::nullopt, std::nullopt, {}};
        for (const auto stage : {ShaderStage::Compute, ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Local, ShaderStage::TessellationControl, ShaderStage::TessellationEvaluation, ShaderStage::Geometry, ShaderStage::Mesh}) {
            request.shader.stage = stage;
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                if ((mask & (1u << bit)) == 0) continue;
                request.context.floatMode = ShaderFloatMode::Decode(1u << bit);
                RecompileCacheKey::Build(request, key);
                const auto hash = RecompileCacheKey::ContextHash(request);
                const auto replay = serializer.Deserialize(serializer.Serialize(request));
                std::vector<std::uint64_t> replayKey;
                RecompileCacheKey::Build(replay.request, replayKey);
                require(replay.request.context.floatMode == request.context.floatMode && replayKey == key && RecompileCacheKey::ContextHash(replay.request) == hash, "a capture changed the shader's float mode or identity");
                require(replay.request.graphics->linkedPrograms[0].floatMode == linked[0].floatMode && replay.request.graphics->linkedPrograms[1].floatMode == linked[1].floatMode, "a capture lost linked stage float modes");
                auto changed = request;
                changed.context.floatMode = ShaderFloatMode{};
                RecompileCacheKey::Build(changed, replayKey);
                require(replayKey != key && RecompileCacheKey::ContextHash(changed) != hash, "shader identity ignored a float control bit");
            }
        }
        request.context.floatMode = ShaderFloatMode{};
        RecompileCacheKey::Build(request, key);
        const auto hash = RecompileCacheKey::ContextHash(request);
        linked[1].floatMode.reset();
        std::vector<std::uint64_t> linkedKey;
        RecompileCacheKey::Build(request, linkedKey);
        require(linkedKey != key && RecompileCacheKey::ContextHash(request) != hash, "shader identity ignored a linked stage mode");
        const auto unknownLinked = serializer.Deserialize(serializer.Serialize(request));
        require(!unknownLinked.request.graphics->linkedPrograms[1].floatMode, "an unknown linked stage mode became known");
        bool rejected = false;
        try {
            static_cast<void>(serializer.Deserialize("NVNQQQoAAAA="));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "a truncated version-10 capture was accepted");
        std::cout << "shader float mode metadata passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

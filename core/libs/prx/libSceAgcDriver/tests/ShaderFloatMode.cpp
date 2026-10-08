#include "CacheKey.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawRegisterKey.hpp"
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
        for (const auto offset : {0x00au, 0x08au, 0x10au}) {
            AgcDriver::QueueState queue;
            const auto absent = AgcDriver::Graphics::DrawRegisterKey(queue, 1);
            queue.shader[offset] = 0;
            const auto zero = AgcDriver::Graphics::DrawRegisterKey(queue, 1);
            require(absent != zero, "the draw key aliases unknown RSRC1 and explicit zero");
            for (const auto bit : {12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 21u, 23u, 26u}) {
                queue.shader[offset] = 1u << bit;
                require(AgcDriver::Graphics::DrawRegisterKey(queue, 1) != zero, "the production draw key ignores a graphics float-mode bit");
            }
            const auto key = AgcDriver::Graphics::DrawRegisterKey(queue, 1);
            queue.shader[0x300] = 0xffffffffu;
            require(AgcDriver::Graphics::DrawRegisterKey(queue, 1) == key, "an unrelated shader register changed the draw key");
            require(AgcDriver::Graphics::DrawRegisterKey(queue, 2) != key, "the draw key ignored the device serial");
        }
        {
            AgcDriver::QueueState queue;
            queue.shader[0x10b] = 2;
            queue.shader[0x10c] = 0x11;
            queue.shader[0x10d] = 0x22;
            const auto used = AgcDriver::Graphics::DrawRegisterKey(queue, 1);
            const auto all = AgcDriver::Graphics::DrawRegisterKey(queue, 1, true);
            queue.shader[0x10d] = 0x33;
            require(AgcDriver::Graphics::DrawRegisterKey(queue, 1) == used, "an unread hull user word changed the optimized draw key");
            require(AgcDriver::Graphics::DrawRegisterKey(queue, 1, true) != all, "the all-user-words diagnostic key ignored an unread word");
            queue.shader[0x10c] = 0x44;
            require(AgcDriver::Graphics::DrawRegisterKey(queue, 1) != used, "a read hull user word was omitted from the draw key");
        }
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

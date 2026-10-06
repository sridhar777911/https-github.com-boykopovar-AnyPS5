#include "Ngs2Test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

static uintptr_t Sampler(uintptr_t system, const std::vector<std::int16_t>& pcm, std::uint32_t repeats) {
    const auto voice = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER));
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, {SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 1, 48000, 0, 0, 0}});
    const Ngs2WaveformBlock block{0, pcm.size() * sizeof(std::int16_t), repeats, 0, static_cast<std::uint32_t>(pcm.size()), 0, 0x55};
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, pcm.data(), 0, 1, &block});
    return voice;
}

static std::vector<std::uint32_t> callbackFlags;
static void APS5_VABI OnBlock(const Ngs2VoiceCallbackInfo* info) {
    Require(info->callback_data == 7 && info->user_data == 0x55);
    callbackFlags.push_back(info->flag);
}

static std::vector<std::int16_t> RenderI16(uintptr_t system) {
    std::vector<std::int16_t> out(Grain, -1);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(std::int16_t), SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 1};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    return out;
}

static void TestErrorsAndInfo() {
    Ngs2SystemOption option{};
    Require(sceNgs2SystemResetOption(&option) == SCE_NGS2_OK);
    Require(option.size == sizeof(option) && option.max_grain_samples == 512 && option.num_grain_samples == 256 && option.sample_rate == 48000);
    Require(sceNgs2SystemQueryBufferSize(&option, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    Require(sceNgs2RackQueryBufferSize(SCE_NGS2_RACK_ID_SAMPLER, nullptr, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);

    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(0x1234, &info, sizeof(info)) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2RackDestroy(0x1234, nullptr) == SCE_NGS2_ERROR_INVALID_RACK_HANDLE);

    const auto system = CreateSystem();
    CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER);
    Require(sceNgs2SystemGetInfo(system, nullptr, sizeof(info)) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info) - 1) == SCE_NGS2_ERROR_INVALID_OUT_SIZE);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK);
    Require(info.system_handle == system && info.uid != 0 && info.rack_count == 1 && info.sample_rate == 48000);
    Require(info.num_grain_samples == Grain && info.max_grain_samples == 512 && info.render_count == 0);

    Ngs2ContextBufferInfo released{};
    Require(sceNgs2SystemDestroy(system, &released) == SCE_NGS2_OK && released.host_buffer != nullptr);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
}

static void TestPcmBlockEnd() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 1);
    std::vector<std::int16_t> pcm;
    for (int i = 0; i < 12; i++) pcm.push_back(static_cast<std::int16_t>(i * 1000 - 4000));
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, master);
    Control(sampler, SCE_NGS2_VOICE_PARAM_CALLBACK, Ngs2VoiceCallbackParam{{}, OnBlock, 7, SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END, 0});
    Require(Flags(sampler) == 0);
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    Require(Flags(sampler) == SCE_NGS2_VOICE_STATE_FLAG_INUSE);

    callbackFlags.clear();
    auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == pcm[i]);
    Require(Flags(sampler) == (SCE_NGS2_VOICE_STATE_FLAG_INUSE | SCE_NGS2_VOICE_STATE_FLAG_PLAYING) && callbackFlags.empty());

    Ngs2SamplerVoiceState state{};
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state) - 8) == SCE_NGS2_ERROR_INVALID_OUT_SIZE);
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.num_decoded_samples == Grain && state.decoded_data_size == Grain * 2 && state.user_data == 0x55);
    Require(state.waveform_data == pcm.data() + Grain);

    out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == (i < 4 ? pcm[Grain + i] : 0));
    Require(callbackFlags.size() == 1 && callbackFlags[0] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END);
    Require(Flags(sampler) == 0);
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.num_decoded_samples == pcm.size() && state.waveform_data == pcm.data() + pcm.size());
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-4f;
}

static bool PanThrows(Ngs2PanWork* work, const Ngs2PanParam* params, std::uint32_t numParams, std::uint32_t format) {
    float out[64] = {};
    try { sceNgs2PanGetVolumeMatrix(work, params, numParams, format, out); } catch (const std::exception&) { return true; }
    return false;
}

static bool PanInitThrows(Ngs2PanWork* work, const float* angles, float unitAngle, std::uint32_t numSpeakers) {
    try { sceNgs2PanInit(work, angles, unitAngle, numSpeakers); } catch (const std::exception&) { return true; }
    return false;
}

struct PanCase {
    std::vector<float> angles;
    float unitAngle;
    std::uint32_t numSpeakers;
    std::vector<Ngs2PanParam> params;
    std::uint32_t format;
    std::vector<float> expected;
};

static void TestPan() {
    const float pi = std::numbers::pi_v<float>;
    Ngs2PanWork work{};
    Require(sceNgs2PanInit(&work, nullptr, 360.0f, 2) == SCE_NGS2_OK);
    Require(work.num_speakers == 2 && work.unit_angle == 360.0f);
    Require(work.speaker_angles[0] == 90.0f && work.speaker_angles[1] == 270.0f && work.speaker_angles[2] == 450.0f);
    Require(sceNgs2PanInit(&work, nullptr, 2.0f * pi, 2) == SCE_NGS2_OK && work.unit_angle == 2.0f * pi);
    Require(work.speaker_angles[0] == 90.0f / 180.0f * pi && work.speaker_angles[1] == 270.0f / 180.0f * pi && work.speaker_angles[2] == 450.0f);
    Require(sceNgs2PanInit(&work, nullptr, 100.0f, 4) == SCE_NGS2_OK && work.unit_angle == 360.0f && work.speaker_angles[4] == 400.0f);
    const float frontBack[] = {-30.0f, 30.0f, 0.0f, -110.0f, 470.0f};
    Require(sceNgs2PanInit(&work, frontBack, 360.0f, 5) == SCE_NGS2_OK);
    const float circle[] = {0.0f, 30.0f, 110.0f, 250.0f, 330.0f, 360.0f};
    for (std::uint32_t i = 0; i < 6; i++) Require(work.speaker_angles[i] == circle[i]);

    const std::vector<PanCase> cases = {
        {{}, 360.0f, 2, {{0, 1, 1, 0}, {45, 1, 1, 0}, {90, 1, 1, 0}, {135, 1, 1, 0}, {180, 1, 1, 0}, {225, 1, 1, 0}, {270, 1, 1, 0}, {315, 1, 1, 0}}, 2,
         {0.707106769f, 0.382683456f, 0.0f, 0.382683456f, 0.707106769f, 0.923879504f, 1.0f, 0.923879504f, 0.707106769f, 0.923879504f, 1.0f, 0.923879504f, 0.707106769f, 0.382683456f, 0.0f, 0.382683456f}},
        {{}, 360.0f, 2, {{30, 0, 1, 0}, {30, -1, 1, 0}, {30, 0.5f, 0.5f, 0}, {30, 3, 1, 0}, {-30, -0.25f, 1, 0}}, 2,
         {0.965925813f, 0.866025448f, 0.39667666f, 0.49999997f, 0.896872759f, 0.965925813f, 0.49999997f, 0.495722443f, 0.866025448f, 0.997858942f}},
        {{}, 360.0f, 2, {{60, 1, 1, 0.5f}}, 1, {0.866025388f}},
        {{}, 360.0f, 2, {{60, 1, 1, 0.5f}}, 6, {0.258819073f, 0.965925813f, 0.0f, 0.0f, 0.0f, 0.0f}},
        {{}, 2.0f * pi, 2, {{0.25f * pi, 1, 1, 0}, {1.5f * pi, 1, 1, 0}}, 2, {0.999965429f, 1.0f, 0.00831161533f, 0.0f}},
        {{}, 360.0f, 4, {{100, 1, 1, 0.25f}}, 6, {0.0f, 0.222520918f, 0.0f, 0.25f, 0.0f, 0.974927902f}},
        {{}, 360.0f, 4, {{100, 1, 1, 0.25f}}, 2, {0.0f, 0.709984899f}},
        {{}, 360.0f, 5, {{70, 1, 0.8f, 0.5f}, {200, 0.5f, 1, 0}}, 6,
         {0.0f, 0.0f, 0.625465155f, 0.270598054f, 0.0f, 0.270598054f, 0.5f, 0.0f, 0.0f, 0.782271147f, 0.498791903f, 0.491533577f}},
        {{}, 360.0f, 5, {{70, 1, 0.8f, 0.5f}}, 1, {0.691666603f}},
        {{}, 360.0f, 7, {{180, 1, 1, 0.25f}}, 8, {0.0f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.707106769f, 0.707106769f}},
        {{}, 360.0f, 7, {{180, 1, 1, 0.25f}}, 6, {0.0f, 0.0f, 0.0f, 0.25f, 0.707106769f, 0.707106769f}},
        {{-pi / 6, pi / 6, 0, -110 * pi / 180, 110 * pi / 180}, 2.0f * pi, 5, {{1.0f, 1, 1, 0}, {-2.5f, 0.2f, 1, 0.1f}}, 6,
         {0.0f, 0.0f, 0.859783232f, 0.582614243f, 0.0f, 0.0f, 0.0f, 0.100000001f, 0.0f, 0.753403723f, 0.510659218f, 0.372568965f}},
        {{-30, 30, 0, -90, 90, -150, 150}, 360.0f, 7, {{-120, 1, 1, 0}}, 1, {0.707106769f}},
        {{-45, 45}, 360.0f, 2, {{10, 1, 1, 0}, {720 + 170, 1, 1, 0}}, 2, {0.57357651f, 0.664795876f, 0.819152057f, 0.747025073f}},
        {{-40, 40, -120, 120}, 360.0f, 4, {{-80, 0.75f, 1, 0}}, 8, {0.69351995f, 0.0746578425f, 0.0f, 0.0f, 0.69351995f, 0.180239946f, 0.0f, 0.0f}},
    };
    for (const auto& panCase : cases) {
        Require(sceNgs2PanInit(&work, panCase.angles.empty() ? nullptr : panCase.angles.data(), panCase.unitAngle, panCase.numSpeakers) == SCE_NGS2_OK);
        std::vector<float> out(panCase.expected.size(), -1.0f);
        Require(sceNgs2PanGetVolumeMatrix(&work, panCase.params.data(), static_cast<std::uint32_t>(panCase.params.size()), panCase.format, out.data()) == SCE_NGS2_OK);
        for (std::size_t i = 0; i < out.size(); i++) Require(std::fabs(out[i] - panCase.expected[i]) < 1e-6f);
    }

    const Ngs2PanParam front{0.0f, 1.0f, 1.0f, 0.0f};
    float out[2] = {};
    Require(sceNgs2PanInit(&work, nullptr, 360.0f, 2) == SCE_NGS2_OK);
    Require(sceNgs2PanGetVolumeMatrix(&work, &front, 1, 2, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    Require(sceNgs2PanGetVolumeMatrix(&work, &front, 0, 2, out) == SCE_NGS2_OK);
    Require(PanThrows(nullptr, &front, 1, 2) && PanThrows(&work, nullptr, 0, 2));
    Require(PanThrows(&work, &front, 1, 4) && PanThrows(&work, &front, 1, 3) && !PanThrows(&work, &front, 1, 8));
    const Ngs2PanParam many[9] = {};
    Require(PanThrows(&work, many, 9, 2) && !PanThrows(&work, many, 8, 2));
    const Ngs2PanParam nanAngle{NAN, 1.0f, 1.0f, 0.0f};
    Require(PanThrows(&work, &nanAngle, 1, 2));
    Require(PanInitThrows(nullptr, nullptr, 360.0f, 2));
    for (std::uint32_t count : {0u, 1u, 3u, 6u, 8u}) Require(PanInitThrows(&work, nullptr, 360.0f, count));
    const float pair[] = {-30.0f, 30.0f};
    Require(PanInitThrows(&work, pair, 180.0f, 2) && !PanInitThrows(&work, pair, 2.0f * pi, 2));
    const float invalid[] = {0.0f, NAN};
    Require(PanInitThrows(&work, invalid, 360.0f, 2));
    const float onUnit[] = {720.0f, 30.0f};
    const float belowZero[] = {-360.0f, 30.0f};
    Require(PanInitThrows(&work, onUnit, 360.0f, 2) && !PanInitThrows(&work, belowZero, 360.0f, 2));
    work.unit_angle = 100.0f;
    Require(PanThrows(&work, &front, 1, 2));
}

static void TestPitchAndRepeat() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 1);
    const std::vector<std::int16_t> pcm{0, 1000, 2000, 3000};
    const auto sampler = Sampler(system, pcm, 1);
    Patch(sampler, master);
    Control(sampler, SCE_NGS2_SAMPLER_VOICE_PARAM_PITCH, Ngs2SamplerVoicePitchParam{{}, 0.5f});
    Control(sampler, SCE_NGS2_VOICE_PARAM_CALLBACK,
            Ngs2VoiceCallbackParam{{}, OnBlock, 7, SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END | SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT, 0});
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);

    callbackFlags.clear();
    const std::int16_t first[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 1500};
    auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == first[i]);
    Require(callbackFlags.size() == 1 && callbackFlags[0] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT);

    const std::int16_t second[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 3000};
    out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == second[i]);
    Require(callbackFlags.size() == 2 && callbackFlags[1] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END);
    Require(Flags(sampler) == 0);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestSubmixerMatrix() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 2);
    const auto submixer = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SUBMIXER));
    Control(submixer, SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP, Ngs2SubmixerVoiceSetupParam{{}, 2, 0});
    Patch(submixer, master);
    const Ngs2VoiceCommand play{2, 0, 4, 0, {.u = SCE_NGS2_VOICE_EVENT_PLAY}};
    Require(sceNgs2VoiceRunCommands(submixer, &play, 1) == SCE_NGS2_OK);

    const std::vector<std::int16_t> pcm(Grain, 16384);
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, submixer);
    const float levels[2] = {1.0f, 0.5f};
    Control(sampler, SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS, Ngs2VoiceMatrixLevelsParam{{}, 0, 2, levels});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_VOLUME, Ngs2VoicePortVolumeParam{{}, 0, 0.5f});
    Require(sceNgs2VoiceRunCommands(sampler, &play, 1) == SCE_NGS2_OK);

    std::vector<float> out(Grain * 2, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i * 2] == 0.25f && out[i * 2 + 1] == 0.125f);

    Ngs2SubmixerVoiceState state{};
    Require(sceNgs2VoiceGetState(submixer, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.voice_state.state_flags == (SCE_NGS2_VOICE_STATE_FLAG_INUSE | SCE_NGS2_VOICE_STATE_FLAG_PLAYING));

    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    for (float sample : out) Require(sample == 0.0f);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static int allocations = 0;
static std::int32_t APS5_VABI Allocate(Ngs2ContextBufferInfo* info) {
    Require(info->host_buffer == nullptr && info->host_buffer_size != 0 && info->user_data == 9);
    info->host_buffer = std::calloc(1, info->host_buffer_size);
    allocations++;
    return SCE_NGS2_OK;
}
static std::int32_t APS5_VABI Release(Ngs2ContextBufferInfo* info) {
    Require(info->host_buffer != nullptr && info->user_data == 9);
    std::free(info->host_buffer);
    allocations--;
    return SCE_NGS2_OK;
}

static void TestSampleRate() {
    const auto system = CreateSystem();
    Require(sceNgs2SystemSetSampleRate(0x1234, 96000) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    bool threw = false;
    try {
        sceNgs2SystemSetSampleRate(system, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Require(threw);
    Require(sceNgs2SystemSetSampleRate(system, 96000) == SCE_NGS2_OK);
    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK && info.sample_rate == 96000);

    const auto master = Mastering(system, 1);
    const std::vector<std::int16_t> pcm{0, 1000, 2000, 3000};
    const auto sampler = Sampler(system, pcm, 1);
    Patch(sampler, master);
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    const std::int16_t expected[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 1500};
    const auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == expected[i]);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestUserData() {
    uintptr_t value = 1;
    Require(sceNgs2SystemSetUserData(0x1234, 5) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2SystemGetUserData(0x1234, &value) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE && value == 1);
    const auto system = CreateSystem();
    Require(sceNgs2SystemGetUserData(system, &value) == SCE_NGS2_OK && value == 0);
    Require(sceNgs2SystemSetUserData(system, 0xfedcba9876543210) == SCE_NGS2_OK);
    Require(sceNgs2SystemGetUserData(system, &value) == SCE_NGS2_OK && value == 0xfedcba9876543210);
    bool threw = false;
    try {
        sceNgs2SystemGetUserData(system, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Require(threw);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestLock() {
    const auto system = CreateSystem();
    Require(sceNgs2SystemLock(0x1234) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2SystemUnlock(0x1234) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Ngs2SystemInfo info{};
    std::thread free([&] { Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK); });
    free.join();

    Require(sceNgs2SystemLock(system) == SCE_NGS2_OK);
    std::atomic<bool> done = false;
    std::thread blocked([&] {
        Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(!done);
    Require(sceNgs2SystemUnlock(system) == SCE_NGS2_OK);
    blocked.join();
    Require(done);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static constexpr float HalfPower = std::numbers::sqrt2_v<float> / 2.0f;

static std::vector<float> MixFrame(std::uint32_t sourceChannels, std::uint32_t sourceChannel, std::uint32_t destChannels) {
    const auto firstBuffer = usedBuffers;
    const auto system = CreateSystem();
    const auto master = Mastering(system, destChannels);
    const auto voice = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER));
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, {SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, sourceChannels, 48000, 0, 0, 0}});
    std::vector<std::int16_t> frames(Grain * sourceChannels, 0);
    for (std::uint32_t i = 0; i < Grain; i++) frames[i * sourceChannels + sourceChannel] = 16384;
    const Ngs2WaveformBlock block{0, frames.size() * sizeof(std::int16_t), 0, 0, Grain, 0, 0};
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, frames.data(), 0, 1, &block});
    Patch(voice, master);
    Event(voice, SCE_NGS2_VOICE_EVENT_PLAY);
    std::vector<float> out(Grain * destChannels, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, destChannels};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
    usedBuffers = firstBuffer;
    return std::vector<float>(out.begin(), out.begin() + destChannels);
}

static void TestDefaultChannelMap() {
    const float half = 0.5f * HalfPower;
    const float quarter = 0.5f * (std::numbers::sqrt2_v<float> / 4.0f);
    Require(MixFrame(1, 0, 1) == std::vector<float>{0.5f});
    Require(MixFrame(1, 0, 2) == (std::vector<float>{half, half}));
    Require(MixFrame(1, 0, 6) == (std::vector<float>{0, 0, 0.5f, 0, 0, 0}));
    Require(MixFrame(1, 0, 8) == (std::vector<float>{0, 0, 0.5f, 0, 0, 0, 0, 0}));
    Require(MixFrame(2, 0, 1) == std::vector<float>{half} && MixFrame(2, 1, 1) == std::vector<float>{half});
    Require(MixFrame(2, 1, 2) == (std::vector<float>{0, 0.5f}));
    Require(MixFrame(2, 1, 6) == (std::vector<float>{0, 0.5f, 0, 0, 0, 0}));
    Require(MixFrame(3, 2, 2) == (std::vector<float>{0, 0}));
    Require(MixFrame(3, 2, 6) == (std::vector<float>{0, 0, 0, 0.5f, 0, 0}));
    Require(MixFrame(4, 2, 1) == std::vector<float>{0.25f});
    Require(MixFrame(4, 2, 2) == (std::vector<float>{half, 0}));
    Require(MixFrame(4, 3, 6) == (std::vector<float>{0, 0, 0, 0, 0, 0.5f}));
    Require(MixFrame(5, 2, 2) == (std::vector<float>{half, half}));
    Require(MixFrame(5, 3, 1) == std::vector<float>{0.25f});
    Require(MixFrame(5, 3, 2) == (std::vector<float>{0, 0}));
    Require(MixFrame(6, 3, 1) == std::vector<float>{0});
    Require(MixFrame(6, 4, 2) == (std::vector<float>{0.25f, 0}));
    Require(MixFrame(6, 5, 8) == (std::vector<float>{0, 0, 0, 0, 0, 0.5f, 0, 0}));
    Require(MixFrame(7, 6, 1) == std::vector<float>{quarter});
    Require(MixFrame(7, 6, 2) == (std::vector<float>{quarter, quarter}));
    Require(MixFrame(7, 6, 6) == (std::vector<float>{0, 0, 0, 0, half, half}));
    Require(MixFrame(7, 6, 8) == (std::vector<float>{0, 0, 0, 0, 0, 0, half, half}));
    Require(MixFrame(8, 6, 2) == (std::vector<float>{0.25f, 0}));
    Require(MixFrame(8, 7, 6) == (std::vector<float>{0, 0, 0, 0, 0, 0.5f}));
    Require(MixFrame(8, 7, 8) == (std::vector<float>{0, 0, 0, 0, 0, 0, 0, 0.5f}));

    const auto system = CreateSystem();
    const auto master = Mastering(system, 2);
    const auto submixer = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SUBMIXER));
    Control(submixer, SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP, Ngs2SubmixerVoiceSetupParam{{}, 4, 0});
    Patch(submixer, master);
    Event(submixer, SCE_NGS2_VOICE_EVENT_PLAY);
    const std::vector<std::int16_t> pcm(Grain, 16384);
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, submixer);
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    std::vector<float> out(Grain * 2, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    bool unsupported = false;
    try { sceNgs2SystemRender(system, &info, 1); } catch (const std::runtime_error&) { unsupported = true; }
    Require(unsupported);
}

static void TestUnsetMatrix() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 2);
    const auto submixer = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SUBMIXER));
    Control(submixer, SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP, Ngs2SubmixerVoiceSetupParam{{}, 2, 0});
    Patch(submixer, master);
    Event(submixer, SCE_NGS2_VOICE_EVENT_PLAY);

    const std::vector<std::int16_t> pcm(Grain, 16384);
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, submixer);
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_VOLUME, Ngs2VoicePortVolumeParam{{}, 0, 0.5f});
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);

    std::vector<float> out(Grain * 2, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    const float spread = 0.25f * HalfPower;
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i * 2] == spread && out[i * 2 + 1] == spread);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);

    const auto stereoSystem = CreateSystem();
    const auto stereoMaster = Mastering(stereoSystem, 2);
    const auto stereo = Voice(CreateRack(stereoSystem, SCE_NGS2_RACK_ID_SAMPLER));
    Control(stereo, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, {SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 2, 48000, 0, 0, 0}});
    std::vector<std::int16_t> frames;
    for (std::uint32_t i = 0; i < Grain; i++) {
        frames.push_back(16384);
        frames.push_back(8192);
    }
    const Ngs2WaveformBlock block{0, frames.size() * sizeof(std::int16_t), 0, 0, Grain, 0, 0};
    Control(stereo, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, frames.data(), 0, 1, &block});
    Patch(stereo, stereoMaster);
    Control(stereo, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Event(stereo, SCE_NGS2_VOICE_EVENT_PLAY);
    Require(sceNgs2SystemRender(stereoSystem, &info, 1) == SCE_NGS2_OK);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i * 2] == 0.5f && out[i * 2 + 1] == 0.25f);
    Require(sceNgs2SystemDestroy(stereoSystem, nullptr) == SCE_NGS2_OK);
}

static void TestAllocator() {
    const Ngs2BufferAllocator allocator{Allocate, Release, 9};
    uintptr_t system = 0;
    Require(sceNgs2SystemCreateWithAllocator(nullptr, &allocator, &system) == SCE_NGS2_OK && allocations == 1);
    uintptr_t sampler = 0;
    uintptr_t master = 0;
    Require(sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_SAMPLER, nullptr, &allocator, &sampler) == SCE_NGS2_OK);
    Require(sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_MASTERING, nullptr, &allocator, &master) == SCE_NGS2_OK && allocations == 3);
    Ngs2ContextBufferInfo released{};
    Require(sceNgs2RackDestroy(sampler, &released) == SCE_NGS2_OK && allocations == 2 && released.host_buffer == nullptr);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK && allocations == 0);
    Require(sceNgs2RackDestroy(master, nullptr) == SCE_NGS2_ERROR_INVALID_RACK_HANDLE);
}

int main() {
    TestErrorsAndInfo();
    TestPcmBlockEnd();
    TestPan();
    TestPitchAndRepeat();
    TestSubmixerMatrix();
    TestSampleRate();
    TestUserData();
    TestLock();
    TestDefaultChannelMap();
    TestUnsetMatrix();
    TestAllocator();
    return 0;
}

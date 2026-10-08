#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "H264TestStream.hpp"
#include "prx/libc/include/General.hpp"

struct DecoderConfigInfo {
    std::uint64_t thisSize;
    std::uint32_t resourceType;
    std::uint32_t codecType;
    std::uint32_t profile;
    std::uint32_t maxLevel;
    std::int32_t maxFrameWidth;
    std::int32_t maxFrameHeight;
    std::int32_t maxDpbFrameCount;
    std::uint32_t decodePipelineDepth;
};

struct DecoderMemoryInfo {
    std::uint64_t thisSize;
    std::uint64_t cpuMemorySize;
    void* cpuMemory;
    std::uint64_t gpuMemorySize;
    void* gpuMemory;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
    std::uint64_t maxFrameBufferSize;
    std::uint32_t frameBufferAlignment;
};

struct InputData {
    std::uint64_t thisSize;
    const std::uint8_t* auData;
    std::uint64_t auSize;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
};

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    bool isAccepted;
};

struct OutputInfo {
    std::uint64_t thisSize;
    bool isValid;
    bool isErrorFrame;
    std::uint8_t pictureCount;
    std::uint32_t codecType;
    std::uint32_t frameWidth;
    std::uint32_t framePitch;
    std::uint32_t frameHeight;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    std::uint32_t frameFormat;
    std::uint32_t framePitchInBytes;
};

struct AvcPictureInfo {
    std::uint64_t thisSize;
    bool isValid;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
    std::uint8_t idrPictureFlag;
    std::uint8_t profileIdc;
    std::uint8_t levelIdc;
    std::uint32_t picWidthInLumaSamples;
    std::uint32_t picHeightInLumaSamples;
};

extern "C" {
int APS5_VABI sceVideodec2QueryDecoderMemoryInfo_nid_postfix(const DecoderConfigInfo*, DecoderMemoryInfo*);
int APS5_VABI sceVideodec2CreateDecoder_nid_postfix(const DecoderConfigInfo*, const DecoderMemoryInfo*, std::uint64_t*);
int APS5_VABI sceVideodec2DeleteDecoder_nid_postfix(std::uint64_t);
int APS5_VABI sceVideodec2Decode_nid_postfix(std::uint64_t, const InputData*, FrameBuffer*, OutputInfo*);
int APS5_VABI sceVideodec2Flush_nid_postfix(std::uint64_t, FrameBuffer*, OutputInfo*);
int APS5_VABI sceVideodec2Reset_nid_postfix(std::uint64_t);
int APS5_VABI sceVideodec2GetPictureInfo_nid_postfix(const OutputInfo*, AvcPictureInfo*, void*);
int APS5_VABI sceVideodec2GetAvcPictureInfo_nid_postfix(const OutputInfo*, AvcPictureInfo*, AvcPictureInfo*);
}

namespace {

using namespace H264TestStream;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename TAction>
void checkThrows(TAction action) {
    bool threw = false;
    try {
        action();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "expected a decoding exception");
}

void testFailures() {
    DecoderConfigInfo config{sizeof(DecoderConfigInfo), 0, 2, 100, 0, static_cast<std::int32_t>(Width), static_cast<std::int32_t>(Height), 4, 1};
    DecoderMemoryInfo memory{sizeof(DecoderMemoryInfo)};
    std::uint64_t handle = 0;
    checkThrows([&] { sceVideodec2CreateDecoder_nid_postfix(&config, &memory, &handle); });
    check(handle == 0, "unsupported codec created a decoder");
    config.codecType = 1;
    config.maxFrameWidth = 0;
    checkThrows([&] { sceVideodec2QueryDecoderMemoryInfo_nid_postfix(&config, &memory); });
    config.maxFrameWidth = Width;
    check(sceVideodec2QueryDecoderMemoryInfo_nid_postfix(&config, &memory) == 0, "memory query failed");
    check(sceVideodec2CreateDecoder_nid_postfix(&config, &memory, &handle) == 0, "decoder creation failed");
    std::vector<std::uint8_t> buffer(memory.maxFrameBufferSize);
    FrameBuffer frame{sizeof(FrameBuffer), buffer.data(), buffer.size(), false};
    OutputInfo output{sizeof(OutputInfo)};
    const std::array<std::uint8_t, 5> malformed{0, 0, 0, 8, 0x65};
    InputData input{sizeof(InputData), malformed.data(), malformed.size(), 0, 0, 0};
    checkThrows([&] { sceVideodec2Decode_nid_postfix(handle, &input, &frame, &output); });
    input.auSize = 2;
    checkThrows([&] { sceVideodec2Decode_nid_postfix(handle, &input, &frame, &output); });
    check(sceVideodec2Reset_nid_postfix(handle) == 0, "reset failed");
    AvcPictureInfo avc{sizeof(AvcPictureInfo)};
    checkThrows([&] { sceVideodec2GetAvcPictureInfo_nid_postfix(nullptr, &avc, nullptr); });
    checkThrows([&] { sceVideodec2GetAvcPictureInfo_nid_postfix(&output, nullptr, nullptr); });
    output.frameBuffer = buffer.data();
    check(sceVideodec2GetAvcPictureInfo_nid_postfix(&output, &avc, nullptr) == 0 && !avc.isValid, "unknown frame buffer reported a picture");
    frame.frameBufferSize = 1;
    checkThrows([&] {
        for (const auto& unit : accessUnits(false)) {
            input.auData = unit.data();
            input.auSize = unit.size();
            sceVideodec2Decode_nid_postfix(handle, &input, &frame, &output);
        }
        sceVideodec2Flush_nid_postfix(handle, &frame, &output);
    });
    check(sceVideodec2DeleteDecoder_nid_postfix(handle) == 0, "delete failed");
}

void testDecode(bool lengthPrefixed) {
    const DecoderConfigInfo config{sizeof(DecoderConfigInfo), 0, 1, 100, 0, static_cast<std::int32_t>(Width), static_cast<std::int32_t>(Height), 4, 1};
    DecoderMemoryInfo memory{sizeof(DecoderMemoryInfo)};
    check(sceVideodec2QueryDecoderMemoryInfo_nid_postfix(&config, &memory) == 0, "memory query failed");
    std::uint64_t handle = 0;
    check(sceVideodec2CreateDecoder_nid_postfix(&config, &memory, &handle) == 0, "decoder creation failed");
    std::vector<std::uint8_t> buffer(memory.maxFrameBufferSize);
    std::size_t pictures = 0;
    const auto take = [&](const OutputInfo& output, const FrameBuffer& frame) {
        check(output.isValid == frame.isAccepted, "frame buffer acceptance disagrees with the output");
        if (!output.isValid) return false;
        check(pictures < PictureHashes.size(), "more pictures than access units");
        check(!output.isErrorFrame && output.frameWidth == Width && output.frameHeight == Height && output.framePitch == 256 && output.frameBuffer == buffer.data(), "unexpected picture geometry");
        AvcPictureInfo info{sizeof(AvcPictureInfo)};
        check(sceVideodec2GetPictureInfo_nid_postfix(&output, &info, nullptr) == 0 && info.isValid, "picture info missing");
        const auto unit = DisplayOrderUnits[pictures];
        check(info.ptsData == 1000 + unit && info.dtsData == unit && info.attachedData == 0xa0 + unit, "picture timestamps out of display order");
        check(info.idrPictureFlag == (pictures == 0 ? 1 : 0) && info.profileIdc == 100, "picture flags mismatch");
        AvcPictureInfo avc{sizeof(AvcPictureInfo)};
        check(sceVideodec2GetAvcPictureInfo_nid_postfix(&output, &avc, nullptr) == 0 && avc.isValid, "avc picture info missing");
        check(avc.ptsData == 1000 + unit && avc.dtsData == unit && avc.attachedData == 0xa0 + unit && avc.idrPictureFlag == (pictures == 0 ? 1 : 0) && avc.profileIdc == 100 && avc.picWidthInLumaSamples == Width && avc.picHeightInLumaSamples == Height, "avc picture info mismatch");
        check(hashNv12(buffer.data(), output.framePitch) == PictureHashes[pictures], "picture " + std::to_string(pictures) + " differs from the reference");
        ++pictures;
        return true;
    };
    const auto units = accessUnits(lengthPrefixed);
    check(units.size() == PictureHashes.size(), "unexpected access unit count");
    for (std::size_t unit = 0; unit < units.size(); ++unit) {
        const InputData input{sizeof(InputData), units[unit].data(), units[unit].size(), 1000 + unit, unit, 0xa0 + unit};
        FrameBuffer frame{sizeof(FrameBuffer), buffer.data(), buffer.size(), false};
        OutputInfo output{sizeof(OutputInfo)};
        check(sceVideodec2Decode_nid_postfix(handle, &input, &frame, &output) == 0, "decode failed");
        take(output, frame);
    }
    for (;;) {
        FrameBuffer frame{sizeof(FrameBuffer), buffer.data(), buffer.size(), false};
        OutputInfo output{sizeof(OutputInfo)};
        check(sceVideodec2Flush_nid_postfix(handle, &frame, &output) == 0, "flush failed");
        if (!take(output, frame)) break;
    }
    check(pictures == PictureHashes.size(), "missing pictures after flush");
    check(sceVideodec2Reset_nid_postfix(handle) == 0, "reset failed");
    check(sceVideodec2DeleteDecoder_nid_postfix(handle) == 0, "delete failed");
    checkThrows([&] { sceVideodec2DeleteDecoder_nid_postfix(handle); });
}

}

int main() {
    try {
        testFailures();
        testDecode(false);
        testDecode(true);
        std::puts("Videodec2 tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

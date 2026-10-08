#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "H264TestStream.hpp"
#include "prx/libc/include/General.hpp"

struct ComputeMemoryInfo {
    std::uint64_t thisSize;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
};

struct ComputeConfigInfo {
    std::uint64_t thisSize;
    std::uint16_t computePipeId;
    std::uint16_t computeQueueId;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};

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
    void* computeQueue;
    std::uint64_t cpuAffinityMask;
    std::int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::int8_t extraDpbFrameCount;
    void* extraConfigInfo;
    bool disableSyncDecodeOutput;
    std::uint32_t maxPendingSyncCount;
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
    std::uint32_t reserved0;
};

struct InputData {
    std::uint64_t thisSize;
    const std::uint8_t* auData;
    std::uint64_t auSize;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
};

struct InputResult {
    std::uint64_t thisSize;
    const std::uint8_t* decodedAu;
    std::uint32_t outputFrameCount;
    std::uint32_t reserved0;
};

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
};

struct OutputInfo {
    std::uint64_t thisSize;
    bool isValid;
    bool isLastFrame;
    bool isErrorFrame;
    std::uint8_t pictureCount;
    std::uint32_t codecType;
    std::uint32_t frameWidth;
    std::uint32_t framePitch;
    std::uint32_t frameHeight;
    bool isDiscardedFrame;
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
    std::uint32_t picWidthInMbsMinus1;
    std::uint32_t picHeightInMapUnitsMinus1;
    std::uint8_t frameMbsOnlyFlag;
    std::uint8_t frameCroppingFlag;
    std::uint32_t frameCropLeftOffset;
    std::uint32_t frameCropRightOffset;
    std::uint32_t frameCropTopOffset;
    std::uint32_t frameCropBottomOffset;
    std::uint8_t remaining[0x78 - 0x48];
};

extern "C" {
int APS5_VABI sceVdecswQueryComputeMemoryInfo(ComputeMemoryInfo*);
int APS5_VABI sceVdecswAllocateComputeQueue(const ComputeConfigInfo*, const ComputeMemoryInfo*, void**);
int APS5_VABI sceVdecswReleaseComputeQueue(void*);
int APS5_VABI sceVdecswQueryDecoderMemoryInfo(const DecoderConfigInfo*, DecoderMemoryInfo*);
int APS5_VABI sceVdecswCreateDecoder(const DecoderConfigInfo*, const DecoderMemoryInfo*, void**);
int APS5_VABI sceVdecswDeleteDecoder(void*);
int APS5_VABI sceVdecswResetDecoder(void*);
int APS5_VABI sceVdecswSetDecodeInput(void*, const InputData*);
int APS5_VABI sceVdecswTrySyncDecodeInput(void*, InputResult*);
int APS5_VABI sceVdecswSetDecodeOutput(void*, FrameBuffer*);
int APS5_VABI sceVdecswTrySyncDecodeOutput(void*, OutputInfo*);
int APS5_VABI sceVdecswFinalizeDecodeSequence(void*);
int APS5_VABI sceVdecswGetAvcPictureInfo(const OutputInfo*, AvcPictureInfo*, AvcPictureInfo*);
}

namespace {

using namespace H264TestStream;

constexpr int StructSize = static_cast<int>(0x81510101);
constexpr int ArgumentPointer = static_cast<int>(0x81510102);
constexpr int DecoderInstance = static_cast<int>(0x81510103);
constexpr int MemoryPointer = static_cast<int>(0x81510105);
constexpr int FrameBufferSize = static_cast<int>(0x81510106);
constexpr int FrameBufferPointer = static_cast<int>(0x81510107);
constexpr int AccessUnitSize = static_cast<int>(0x8151010D);
constexpr int AccessUnitPointer = static_cast<int>(0x8151010E);
constexpr int OutputBufferFull = static_cast<int>(0x81510114);
constexpr int InputQueueEmpty = static_cast<int>(0x81510116);
constexpr int DecodePending = static_cast<int>(0x81510117);
constexpr int OutputBufferEmpty = static_cast<int>(0x81510118);
constexpr int ConfigInfo = static_cast<int>(0x81510200);
constexpr int ComputePipeId = static_cast<int>(0x81510201);
constexpr int ComputeQueueId = static_cast<int>(0x81510202);

constexpr std::uint32_t Pitch = 64;
constexpr std::uint32_t PaddedHeight = 48;

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

DecoderConfigInfo Config(std::uint64_t size) {
    DecoderConfigInfo config{};
    config.thisSize = size;
    config.codecType = 1;
    config.profile = 100;
    config.maxFrameWidth = static_cast<std::int32_t>(Width);
    config.maxFrameHeight = static_cast<std::int32_t>(Height);
    config.maxDpbFrameCount = 4;
    config.decodePipelineDepth = 1;
    return config;
}

void testQueries() {
    ComputeMemoryInfo compute{sizeof(ComputeMemoryInfo), 0, nullptr};
    check(sceVdecswQueryComputeMemoryInfo(nullptr) == ArgumentPointer, "null compute memory info accepted");
    compute.thisSize = 8;
    check(sceVdecswQueryComputeMemoryInfo(&compute) == StructSize, "bad compute memory info size accepted");
    compute.thisSize = sizeof(ComputeMemoryInfo);
    check(sceVdecswQueryComputeMemoryInfo(&compute) == 0 && compute.cpuGpuMemorySize != 0, "compute memory query failed");
    std::vector<std::uint8_t> work(compute.cpuGpuMemorySize);
    compute.cpuGpuMemory = work.data();
    ComputeConfigInfo queueConfig{sizeof(ComputeConfigInfo), 0, 0, false, 0, 0};
    void* queue = nullptr;
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, nullptr) == ArgumentPointer, "null queue accepted");
    queueConfig.computePipeId = 5;
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, &queue) == ComputePipeId, "compute pipe 5 accepted");
    queueConfig.computePipeId = 4;
    queueConfig.computeQueueId = 8;
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, &queue) == ComputeQueueId, "compute queue 8 accepted");
    queueConfig.computeQueueId = 7;
    queueConfig.reserved1 = 1;
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, &queue) == ConfigInfo, "reserved field accepted");
    queueConfig.reserved1 = 0;
    compute.cpuGpuMemory = nullptr;
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, &queue) == MemoryPointer, "null compute memory accepted");
    compute.cpuGpuMemory = work.data();
    check(sceVdecswAllocateComputeQueue(&queueConfig, &compute, &queue) == 0 && queue == work.data(), "compute queue allocation failed");
    check(sceVdecswReleaseComputeQueue(queue) == 0, "compute queue release failed");

    DecoderMemoryInfo memory{};
    memory.thisSize = sizeof(DecoderMemoryInfo);
    for (const auto size : {std::uint64_t{0x50}, std::uint64_t{0x48}}) {
        const auto config = Config(size);
        check(sceVdecswQueryDecoderMemoryInfo(&config, &memory) == 0, "decoder memory query failed");
        check(memory.maxFrameBufferSize == static_cast<std::uint64_t>(Pitch) * PaddedHeight * 3 / 2 && memory.frameBufferAlignment == 256, "unexpected frame buffer requirements");
    }
    auto config = Config(0x40);
    check(sceVdecswQueryDecoderMemoryInfo(&config, &memory) == StructSize, "bad config size accepted");
    check(sceVdecswQueryDecoderMemoryInfo(nullptr, &memory) == ArgumentPointer, "null config accepted");
    config = Config(sizeof(DecoderConfigInfo));
    config.maxFrameHeight = 0;
    checkThrows([&] { sceVdecswQueryDecoderMemoryInfo(&config, &memory); });
}

void testFailures() {
    auto config = Config(sizeof(DecoderConfigInfo));
    DecoderMemoryInfo memory{};
    memory.thisSize = sizeof(DecoderMemoryInfo);
    void* handle = nullptr;
    config.codecType = 2;
    checkThrows([&] { sceVdecswCreateDecoder(&config, &memory, &handle); });
    check(handle == nullptr, "unsupported codec created a decoder");
    config.codecType = 1;
    memory.thisSize = 8;
    check(sceVdecswCreateDecoder(&config, &memory, &handle) == StructSize, "bad memory info size accepted");
    memory.thisSize = sizeof(DecoderMemoryInfo);
    check(sceVdecswCreateDecoder(&config, &memory, nullptr) == ArgumentPointer, "null handle accepted");
    check(sceVdecswCreateDecoder(&config, &memory, &handle) == 0 && handle != nullptr, "decoder creation failed");

    int unknown = 0;
    InputData input{sizeof(InputData), nullptr, 4, 0, 0, 0};
    check(sceVdecswSetDecodeInput(&unknown, &input) == DecoderInstance, "unknown decoder accepted");
    check(sceVdecswDeleteDecoder(&unknown) == DecoderInstance, "unknown decoder deleted");
    check(sceVdecswSetDecodeInput(handle, nullptr) == ArgumentPointer, "null input accepted");
    check(sceVdecswSetDecodeInput(handle, &input) == AccessUnitPointer, "null access unit accepted");
    const std::array<std::uint8_t, 5> malformed{0, 0, 0, 8, 0x65};
    input.auData = malformed.data();
    input.auSize = 0;
    check(sceVdecswSetDecodeInput(handle, &input) == AccessUnitSize, "empty access unit accepted");
    input.thisSize = 8;
    check(sceVdecswSetDecodeInput(handle, &input) == StructSize, "bad input size accepted");
    input.thisSize = sizeof(InputData);
    input.auSize = malformed.size();
    checkThrows([&] { sceVdecswSetDecodeInput(handle, &input); });

    InputResult result{sizeof(InputResult), nullptr, 0, 0};
    check(sceVdecswTrySyncDecodeInput(handle, &result) == InputQueueEmpty, "empty input queue synced");
    check(sceVdecswTrySyncDecodeInput(handle, nullptr) == ArgumentPointer, "null input result accepted");

    std::vector<std::uint8_t> buffer(memory.maxFrameBufferSize + 1);
    FrameBuffer frame{sizeof(FrameBuffer), nullptr, buffer.size()};
    check(sceVdecswSetDecodeOutput(handle, &frame) == FrameBufferPointer, "null frame buffer accepted");
    frame.frameBuffer = buffer.data();
    frame.frameBufferSize = 0;
    check(sceVdecswSetDecodeOutput(handle, &frame) == FrameBufferSize, "empty frame buffer accepted");
    frame.frameBufferSize = buffer.size();
    OutputInfo output{sizeof(OutputInfo)};
    check(sceVdecswTrySyncDecodeOutput(handle, &output) == OutputBufferEmpty, "output synced without a frame buffer");
    check(sceVdecswSetDecodeOutput(handle, &frame) == 0, "frame buffer rejected");
    check(sceVdecswSetDecodeOutput(handle, &frame) == OutputBufferFull, "second frame buffer accepted");
    check(sceVdecswTrySyncDecodeOutput(handle, &output) == DecodePending && !output.isValid, "output ready without input");
    output.thisSize = 8;
    check(sceVdecswTrySyncDecodeOutput(handle, &output) == StructSize, "bad output info size accepted");

    AvcPictureInfo avc{};
    avc.thisSize = sizeof(AvcPictureInfo);
    output = OutputInfo{sizeof(OutputInfo)};
    output.frameBuffer = buffer.data();
    check(sceVdecswGetAvcPictureInfo(nullptr, &avc, nullptr) == ArgumentPointer, "null output info accepted");
    check(sceVdecswGetAvcPictureInfo(&output, &avc, nullptr) == 0 && !avc.isValid, "unknown frame buffer reported a picture");
    check(sceVdecswResetDecoder(handle) == 0, "reset failed");
    check(sceVdecswDeleteDecoder(handle) == 0, "delete failed");
    check(sceVdecswDeleteDecoder(handle) == DecoderInstance, "deleted decoder deleted again");
}

void testDecode(bool lengthPrefixed, std::uint64_t outputSize) {
    const auto config = Config(sizeof(DecoderConfigInfo));
    DecoderMemoryInfo memory{};
    memory.thisSize = sizeof(DecoderMemoryInfo);
    check(sceVdecswQueryDecoderMemoryInfo(&config, &memory) == 0, "memory query failed");
    void* handle = nullptr;
    check(sceVdecswCreateDecoder(&config, &memory, &handle) == 0, "decoder creation failed");
    std::vector<std::uint8_t> buffer(memory.maxFrameBufferSize);
    std::size_t pictures = 0;
    bool last = false;
    bool outputPending = false;
    const auto drain = [&] {
        for (;;) {
            FrameBuffer frame{sizeof(FrameBuffer), buffer.data(), buffer.size()};
            if (!outputPending) check(sceVdecswSetDecodeOutput(handle, &frame) == 0, "frame buffer rejected");
            outputPending = true;
            OutputInfo output{outputSize};
            const int result = sceVdecswTrySyncDecodeOutput(handle, &output);
            if (result == DecodePending) {
                check(!output.isValid, "pending output marked valid");
                check(sceVdecswSetDecodeOutput(handle, &frame) == OutputBufferFull, "pending frame buffer dropped");
                return;
            }
            outputPending = false;
            check(result == 0, "output sync failed");
            if (!output.isValid) {
                check(output.isLastFrame && pictures == PictureHashes.size(), "sequence ended early");
                last = true;
                return;
            }
            check(pictures < PictureHashes.size(), "more pictures than access units");
            check(!output.isErrorFrame && output.pictureCount == 1 && output.codecType == 1, "unexpected picture flags");
            check(output.frameWidth == Width && output.framePitch == Pitch && output.frameHeight == PaddedHeight, "unexpected picture geometry");
            check(output.frameBuffer == buffer.data() && output.frameBufferSize == memory.maxFrameBufferSize, "unexpected frame buffer");
            if (outputSize == sizeof(OutputInfo)) check(output.frameFormat == 0 && output.framePitchInBytes == Pitch, "unexpected frame format");
            check(hashNv12(buffer.data(), Pitch, PaddedHeight) == PictureHashes[pictures], "picture " + std::to_string(pictures) + " differs from the reference");
            for (std::uint32_t row = Height; row < PaddedHeight; ++row) {
                check(std::memcmp(buffer.data() + row * Pitch, buffer.data() + (Height - 1) * Pitch, Width) == 0, "luma padding is not the last row");
            }
            AvcPictureInfo avc{};
            avc.thisSize = sizeof(AvcPictureInfo);
            check(sceVdecswGetAvcPictureInfo(&output, &avc, nullptr) == 0 && avc.isValid, "avc picture info missing");
            const auto unit = DisplayOrderUnits[pictures];
            check(avc.ptsData == 1000 + unit && avc.dtsData == unit && avc.attachedData == 0xa0 + unit, "picture timestamps out of display order");
            check(avc.idrPictureFlag == (pictures == 0 ? 1 : 0) && avc.profileIdc == 100, "picture flags mismatch");
            check(avc.picWidthInMbsMinus1 == 3 && avc.picHeightInMapUnitsMinus1 == 2 && avc.frameMbsOnlyFlag == 1, "picture size mismatch");
            check(avc.frameCroppingFlag == 1 && avc.frameCropLeftOffset == 0 && avc.frameCropRightOffset == 0 && avc.frameCropTopOffset == 0 && avc.frameCropBottomOffset == 4, "picture cropping mismatch");
            ++pictures;
            if (output.isLastFrame) {
                check(pictures == PictureHashes.size(), "last frame flagged early");
                last = true;
                return;
            }
        }
    };
    const auto units = accessUnits(lengthPrefixed);
    check(units.size() == PictureHashes.size(), "unexpected access unit count");
    std::uint32_t reported = 0;
    for (std::size_t unit = 0; unit < units.size(); ++unit) {
        const InputData input{sizeof(InputData), units[unit].data(), units[unit].size(), 1000 + unit, unit, 0xa0 + unit};
        check(sceVdecswSetDecodeInput(handle, &input) == 0, "decode input failed");
        InputResult result{sizeof(InputResult), nullptr, 0, 0};
        check(sceVdecswTrySyncDecodeInput(handle, &result) == 0 && result.decodedAu == units[unit].data(), "input result mismatch");
        reported += result.outputFrameCount;
        check(sceVdecswTrySyncDecodeInput(handle, &result) == InputQueueEmpty, "input result reported twice");
        drain();
        check(!last, "sequence ended before finalizing");
    }
    check(reported < PictureHashes.size(), "every picture left the decoder before finalizing");
    check(sceVdecswFinalizeDecodeSequence(handle) == 0, "finalize failed");
    check(sceVdecswFinalizeDecodeSequence(handle) == 0, "second finalize failed");
    drain();
    check(last && pictures == PictureHashes.size(), "missing pictures after finalizing");
    FrameBuffer frame{sizeof(FrameBuffer), buffer.data(), buffer.size()};
    check(sceVdecswSetDecodeOutput(handle, &frame) == 0, "frame buffer rejected after the last frame");
    OutputInfo output{outputSize};
    check(sceVdecswTrySyncDecodeOutput(handle, &output) == 0 && !output.isValid && output.isLastFrame, "finished sequence returned a picture");
    const InputData again{sizeof(InputData), units[0].data(), units[0].size(), 0, 0, 0};
    checkThrows([&] { sceVdecswSetDecodeInput(handle, &again); });
    check(sceVdecswResetDecoder(handle) == 0, "reset failed");
    check(sceVdecswSetDecodeInput(handle, &again) == 0, "decode after reset failed");
    check(sceVdecswDeleteDecoder(handle) == 0, "delete failed");
}

}

int main() {
    try {
        testQueries();
        testFailures();
        testDecode(false, sizeof(OutputInfo));
        testDecode(true, sizeof(OutputInfo));
        testDecode(false, 0x30);
        std::puts("Vdecsw tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

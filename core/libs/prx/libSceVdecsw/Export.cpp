#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int VDECSW_ERROR_STRUCT_SIZE = static_cast<int>(0x81510101);
constexpr int VDECSW_ERROR_ARGUMENT_POINTER = static_cast<int>(0x81510102);
constexpr int VDECSW_ERROR_DECODER_INSTANCE = static_cast<int>(0x81510103);
constexpr int VDECSW_ERROR_MEMORY_POINTER = static_cast<int>(0x81510105);
constexpr int VDECSW_ERROR_FRAME_BUFFER_SIZE = static_cast<int>(0x81510106);
constexpr int VDECSW_ERROR_FRAME_BUFFER_POINTER = static_cast<int>(0x81510107);
constexpr int VDECSW_ERROR_ACCESS_UNIT_SIZE = static_cast<int>(0x8151010D);
constexpr int VDECSW_ERROR_ACCESS_UNIT_POINTER = static_cast<int>(0x8151010E);
constexpr int VDECSW_ERROR_OUTPUT_BUFFER_FULL = static_cast<int>(0x81510114);
constexpr int VDECSW_ERROR_INPUT_QUEUE_EMPTY = static_cast<int>(0x81510116);
constexpr int VDECSW_ERROR_DECODE_PENDING = static_cast<int>(0x81510117);
constexpr int VDECSW_ERROR_OUTPUT_BUFFER_EMPTY = static_cast<int>(0x81510118);
constexpr int VDECSW_ERROR_CONFIG_INFO = static_cast<int>(0x81510200);
constexpr int VDECSW_ERROR_COMPUTE_PIPE_ID = static_cast<int>(0x81510201);
constexpr int VDECSW_ERROR_COMPUTE_QUEUE_ID = static_cast<int>(0x81510202);

constexpr std::uint32_t CodecAvc = 1;
constexpr std::uint64_t WorkMemoryBytes = 16u << 20u;
constexpr std::uint32_t PitchAlignment = 64;
constexpr std::uint32_t HeightAlignment = 16;
constexpr std::uint32_t FrameBufferAlignment = 256;

struct ComputeMemoryInfo {
    std::uint64_t thisSize;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
};
static_assert(sizeof(ComputeMemoryInfo) == 0x18);

struct ComputeConfigInfo {
    std::uint64_t thisSize;
    std::uint16_t computePipeId;
    std::uint16_t computeQueueId;
    bool checkMemoryType;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfigInfo) == 0x10);

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
static_assert(sizeof(DecoderConfigInfo) == 0x50);
constexpr std::uint64_t DecoderConfigInfoV1Size = offsetof(DecoderConfigInfo, disableSyncDecodeOutput);
static_assert(DecoderConfigInfoV1Size == 0x48);

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
static_assert(sizeof(DecoderMemoryInfo) == 0x48);

struct InputData {
    std::uint64_t thisSize;
    const std::uint8_t* auData;
    std::uint64_t auSize;
    std::uint64_t ptsData;
    std::uint64_t dtsData;
    std::uint64_t attachedData;
};
static_assert(sizeof(InputData) == 0x30);

struct InputResult {
    std::uint64_t thisSize;
    const std::uint8_t* decodedAu;
    std::uint32_t outputFrameCount;
    std::uint32_t reserved0;
};
static_assert(sizeof(InputResult) == 0x18);

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
};
static_assert(sizeof(FrameBuffer) == 0x18);

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
static_assert(sizeof(OutputInfo) == 0x38);
constexpr std::uint64_t OutputInfoV1Size = offsetof(OutputInfo, frameFormat);
static_assert(OutputInfoV1Size == 0x30);

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
static_assert(offsetof(AvcPictureInfo, frameCropBottomOffset) == 0x44);
static_assert(sizeof(AvcPictureInfo) == 0x78);

struct Picture {
    std::uint64_t pts = 0;
    std::uint64_t dts = 0;
    std::uint64_t attached = 0;
    bool idr = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t profile = 0;
    std::uint8_t level = 0;
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct CompletedInput {
    const std::uint8_t* au = nullptr;
    std::uint32_t frames = 0;
};

struct ReadyFrame {
    FramePointer frame;
    Picture picture;
};

struct Decoder {
    std::mutex mutex;
    AVCodecContext* context = nullptr;
    SwsContext* scaler = nullptr;
    std::map<std::int64_t, Picture> inputs;
    std::int64_t nextKey = 0;
    std::deque<CompletedInput> completed;
    std::deque<ReadyFrame> ready;
    std::optional<FrameBuffer> output;
    std::map<const void*, Picture> pictures;
    std::vector<std::uint8_t> annexB;
    bool finalized = false;

    ~Decoder() {
        sws_freeContext(scaler);
        avcodec_free_context(&context);
    }
};

std::mutex registryMutex;
std::map<const void*, std::shared_ptr<Decoder>> decoders;

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t FrameBytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::uint64_t>(AlignUp(width, PitchAlignment)) * AlignUp(height, HeightAlignment) * 3 / 2;
}

bool ValidConfigSize(const DecoderConfigInfo* config) {
    return config->thisSize == sizeof(DecoderConfigInfo) || config->thisSize == DecoderConfigInfoV1Size;
}

std::shared_ptr<Decoder> FindDecoder(const void* handle) {
    std::lock_guard guard(registryMutex);
    const auto found = decoders.find(handle);
    return found != decoders.end() ? found->second : nullptr;
}

void OpenCodec(Decoder& decoder) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) throw std::runtime_error("Vdecsw: H.264 decoder is unavailable");
    decoder.context = avcodec_alloc_context3(codec);
    if (decoder.context == nullptr) throw std::runtime_error("Vdecsw: cannot allocate codec context");
    decoder.context->err_recognition = AV_EF_EXPLODE;
    decoder.context->thread_count = 0;
    decoder.context->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(decoder.context, codec, nullptr) < 0) {
        avcodec_free_context(&decoder.context);
        throw std::runtime_error("Vdecsw: cannot open H.264 decoder");
    }
}

const std::vector<std::uint8_t>& ToAnnexB(Decoder& decoder, const std::uint8_t* data, std::size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        decoder.annexB.assign(data, data + size);
        return decoder.annexB;
    }
    decoder.annexB.clear();
    for (std::size_t index = 0; index < size;) {
        if (size - index < 4) throw std::runtime_error("Vdecsw: truncated NAL length");
        const std::size_t length = (static_cast<std::size_t>(data[index]) << 24u) | (static_cast<std::size_t>(data[index + 1]) << 16u) | (static_cast<std::size_t>(data[index + 2]) << 8u) | data[index + 3];
        if (length == 0 || length > size - index - 4) throw std::runtime_error("Vdecsw: invalid NAL length");
        static constexpr std::uint8_t startCode[4] = {0, 0, 0, 1};
        decoder.annexB.insert(decoder.annexB.end(), startCode, startCode + 4);
        decoder.annexB.insert(decoder.annexB.end(), data + index + 4, data + index + 4 + length);
        index += 4 + length;
    }
    return decoder.annexB;
}

std::uint32_t Receive(Decoder& decoder) {
    std::uint32_t frames = 0;
    for (;;) {
        FramePointer frame(av_frame_alloc());
        if (!frame) throw std::runtime_error("Vdecsw: cannot allocate frame");
        const int result = avcodec_receive_frame(decoder.context, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return frames;
        if (result < 0 || frame->decode_error_flags != 0) throw std::runtime_error("Vdecsw: frame decoding failed");
        if ((frame->flags & AV_FRAME_FLAG_INTERLACED) != 0) throw std::runtime_error("Vdecsw: interlaced pictures are not supported");
        if (frame->width <= 0 || frame->height <= 0 || frame->width % 2 != 0 || frame->height % 2 != 0) throw std::runtime_error("Vdecsw: unsupported frame dimensions");
        const auto found = decoder.inputs.find(frame->pts);
        if (found == decoder.inputs.end()) throw std::runtime_error("Vdecsw: missing picture timestamps");
        Picture picture = found->second;
        decoder.inputs.erase(found);
        picture.idr = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
        picture.width = static_cast<std::uint32_t>(frame->width);
        picture.height = static_cast<std::uint32_t>(frame->height);
        if (decoder.context->profile > 0) picture.profile = static_cast<std::uint8_t>(decoder.context->profile & 0xff);
        if (decoder.context->level > 0) picture.level = static_cast<std::uint8_t>(decoder.context->level);
        decoder.ready.push_back({std::move(frame), picture});
        ++frames;
    }
}

std::uint32_t Send(Decoder& decoder, const AVPacket* packet) {
    if (avcodec_send_packet(decoder.context, packet) < 0) throw std::runtime_error("Vdecsw: packet submission failed");
    return Receive(decoder);
}

void WriteNv12(Decoder& decoder, const AVFrame& frame, std::uint8_t* target) {
    const auto width = static_cast<std::uint32_t>(frame.width);
    const auto height = static_cast<std::uint32_t>(frame.height);
    const auto pitch = AlignUp(width, PitchAlignment);
    const auto rows = AlignUp(height, HeightAlignment);
    std::uint8_t* luma = target;
    std::uint8_t* chroma = target + static_cast<std::size_t>(pitch) * rows;
    const auto format = static_cast<AVPixelFormat>(frame.format);
    if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
        for (std::uint32_t row = 0; row < height; ++row) std::memcpy(luma + static_cast<std::size_t>(row) * pitch, frame.data[0] + static_cast<std::ptrdiff_t>(row) * frame.linesize[0], width);
        for (std::uint32_t row = 0; row < height / 2; ++row) {
            const auto* u = frame.data[1] + static_cast<std::ptrdiff_t>(row) * frame.linesize[1];
            const auto* v = frame.data[2] + static_cast<std::ptrdiff_t>(row) * frame.linesize[2];
            auto* uv = chroma + static_cast<std::size_t>(row) * pitch;
            for (std::uint32_t column = 0; column < width / 2; ++column) {
                uv[column * 2] = u[column];
                uv[column * 2 + 1] = v[column];
            }
        }
    } else {
        std::uint8_t* planes[4] = {luma, chroma, nullptr, nullptr};
        const int strides[4] = {static_cast<int>(pitch), static_cast<int>(pitch), 0, 0};
        decoder.scaler = sws_getCachedContext(decoder.scaler, frame.width, frame.height, format, frame.width, frame.height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (decoder.scaler == nullptr || sws_scale(decoder.scaler, frame.data, frame.linesize, 0, frame.height, planes, strides) != frame.height) throw std::runtime_error("Vdecsw: NV12 conversion failed");
    }
    for (std::uint32_t row = height; row < rows; ++row) std::memcpy(luma + static_cast<std::size_t>(row) * pitch, luma + static_cast<std::size_t>(height - 1) * pitch, width);
    for (std::uint32_t row = height / 2; row < rows / 2; ++row) std::memcpy(chroma + static_cast<std::size_t>(row) * pitch, chroma + static_cast<std::size_t>(height / 2 - 1) * pitch, width);
}

void ClearOutputInfo(OutputInfo* output) {
    output->isValid = false;
    output->isLastFrame = false;
    output->isErrorFrame = false;
    output->isDiscardedFrame = false;
    output->pictureCount = 0;
}

void EmitFrame(Decoder& decoder, const FrameBuffer& target, OutputInfo* output) {
    ReadyFrame ready = std::move(decoder.ready.front());
    const auto& picture = ready.picture;
    const auto pitch = AlignUp(picture.width, PitchAlignment);
    const auto rows = AlignUp(picture.height, HeightAlignment);
    if (FrameBytes(picture.width, picture.height) > target.frameBufferSize) throw std::runtime_error("Vdecsw: frame buffer is smaller than the decoded picture");
    decoder.ready.pop_front();
    WriteNv12(decoder, *ready.frame, static_cast<std::uint8_t*>(target.frameBuffer));
    output->isValid = true;
    output->isLastFrame = decoder.finalized && decoder.ready.empty();
    output->pictureCount = 1;
    output->codecType = CodecAvc;
    output->frameWidth = AlignUp(picture.width, HeightAlignment);
    output->framePitch = pitch;
    output->frameHeight = rows;
    output->frameBuffer = target.frameBuffer;
    output->frameBufferSize = FrameBytes(picture.width, picture.height);
    if (output->thisSize == sizeof(OutputInfo)) {
        output->frameFormat = 0;
        output->framePitchInBytes = pitch;
    }
    decoder.pictures[target.frameBuffer] = picture;
}

}

extern "C" {

int APS5_VABI sceVdecswQueryComputeMemoryInfo(ComputeMemoryInfo* info) {
    if (info == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (info->thisSize != sizeof(ComputeMemoryInfo)) return VDECSW_ERROR_STRUCT_SIZE;
    info->cpuGpuMemorySize = WorkMemoryBytes;
    info->cpuGpuMemory = nullptr;
    return 0;
}

int APS5_VABI sceVdecswAllocateComputeQueue(const ComputeConfigInfo* config, const ComputeMemoryInfo* memory, void** queue) {
    if (config == nullptr || memory == nullptr || queue == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (config->thisSize != sizeof(ComputeConfigInfo) || memory->thisSize != sizeof(ComputeMemoryInfo)) return VDECSW_ERROR_STRUCT_SIZE;
    if (config->reserved0 != 0 || config->reserved1 != 0) return VDECSW_ERROR_CONFIG_INFO;
    if (config->computePipeId > 4) return VDECSW_ERROR_COMPUTE_PIPE_ID;
    if (config->computeQueueId > 7) return VDECSW_ERROR_COMPUTE_QUEUE_ID;
    if (memory->cpuGpuMemory == nullptr) return VDECSW_ERROR_MEMORY_POINTER;
    *queue = memory->cpuGpuMemory;
    return 0;
}

int APS5_VABI sceVdecswReleaseComputeQueue(void* queue) {
    (void)queue;
    return 0;
}

int APS5_VABI sceVdecswQueryDecoderMemoryInfo(const DecoderConfigInfo* config, DecoderMemoryInfo* memory) {
    if (config == nullptr || memory == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (!ValidConfigSize(config) || memory->thisSize != sizeof(DecoderMemoryInfo)) return VDECSW_ERROR_STRUCT_SIZE;
    if (config->maxFrameWidth <= 0 || config->maxFrameHeight <= 0) throw std::runtime_error("Vdecsw: frame dimensions are not positive");
    memory->cpuMemorySize = WorkMemoryBytes;
    memory->cpuMemory = nullptr;
    memory->gpuMemorySize = WorkMemoryBytes;
    memory->gpuMemory = nullptr;
    memory->cpuGpuMemorySize = WorkMemoryBytes;
    memory->cpuGpuMemory = nullptr;
    memory->maxFrameBufferSize = FrameBytes(static_cast<std::uint32_t>(config->maxFrameWidth), static_cast<std::uint32_t>(config->maxFrameHeight));
    memory->frameBufferAlignment = FrameBufferAlignment;
    return 0;
}

int APS5_VABI sceVdecswCreateDecoder(const DecoderConfigInfo* config, const DecoderMemoryInfo* memory, void** handle) {
    if (config == nullptr || memory == nullptr || handle == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (!ValidConfigSize(config) || memory->thisSize != sizeof(DecoderMemoryInfo)) return VDECSW_ERROR_STRUCT_SIZE;
    if (config->codecType != CodecAvc) throw std::runtime_error("Vdecsw: only H.264 is supported");
    auto decoder = std::make_shared<Decoder>();
    OpenCodec(*decoder);
    std::lock_guard guard(registryMutex);
    *handle = decoder.get();
    decoders.emplace(decoder.get(), std::move(decoder));
    return 0;
}

int APS5_VABI sceVdecswDeleteDecoder(void* handle) {
    std::lock_guard guard(registryMutex);
    if (decoders.erase(handle) == 0) return VDECSW_ERROR_DECODER_INSTANCE;
    return 0;
}

int APS5_VABI sceVdecswResetDecoder(void* handle) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    std::lock_guard guard(decoder->mutex);
    avcodec_flush_buffers(decoder->context);
    decoder->inputs.clear();
    decoder->completed.clear();
    decoder->ready.clear();
    decoder->output.reset();
    decoder->pictures.clear();
    decoder->finalized = false;
    return 0;
}

int APS5_VABI sceVdecswSetDecodeInput(void* handle, const InputData* input) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    if (input == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (input->thisSize != sizeof(InputData)) return VDECSW_ERROR_STRUCT_SIZE;
    if (input->auData == nullptr) return VDECSW_ERROR_ACCESS_UNIT_POINTER;
    if (input->auSize == 0) return VDECSW_ERROR_ACCESS_UNIT_SIZE;
    if (input->auSize > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Vdecsw: access unit is too large");
    std::lock_guard guard(decoder->mutex);
    if (decoder->finalized) throw std::runtime_error("Vdecsw: reset required after finalizing the sequence");
    const auto& unit = ToAnnexB(*decoder, input->auData, static_cast<std::size_t>(input->auSize));
    const std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) throw std::runtime_error("Vdecsw: cannot allocate packet");
    if (av_new_packet(packet.get(), static_cast<int>(unit.size())) < 0) throw std::runtime_error("Vdecsw: cannot allocate packet data");
    std::memcpy(packet->data, unit.data(), unit.size());
    packet->pts = decoder->nextKey;
    decoder->inputs.emplace(decoder->nextKey++, Picture{input->ptsData, input->dtsData, input->attachedData});
    const auto frames = Send(*decoder, packet.get());
    decoder->completed.push_back({input->auData, frames});
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeInput(void* handle, InputResult* result) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    if (result == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    std::lock_guard guard(decoder->mutex);
    if (decoder->completed.empty()) return VDECSW_ERROR_INPUT_QUEUE_EMPTY;
    const auto completed = decoder->completed.front();
    decoder->completed.pop_front();
    result->decodedAu = completed.au;
    result->outputFrameCount = completed.frames;
    result->reserved0 = 0;
    return 0;
}

int APS5_VABI sceVdecswSetDecodeOutput(void* handle, FrameBuffer* frame) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    if (frame == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (frame->thisSize != sizeof(FrameBuffer)) return VDECSW_ERROR_STRUCT_SIZE;
    if (frame->frameBuffer == nullptr) return VDECSW_ERROR_FRAME_BUFFER_POINTER;
    if (frame->frameBufferSize == 0) return VDECSW_ERROR_FRAME_BUFFER_SIZE;
    std::lock_guard guard(decoder->mutex);
    if (decoder->output) return VDECSW_ERROR_OUTPUT_BUFFER_FULL;
    decoder->output = *frame;
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeOutput(void* handle, OutputInfo* output) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    if (output == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (output->thisSize != sizeof(OutputInfo) && output->thisSize != OutputInfoV1Size) return VDECSW_ERROR_STRUCT_SIZE;
    ClearOutputInfo(output);
    std::lock_guard guard(decoder->mutex);
    if (!decoder->output) return VDECSW_ERROR_OUTPUT_BUFFER_EMPTY;
    if (decoder->ready.empty()) {
        if (!decoder->finalized) return VDECSW_ERROR_DECODE_PENDING;
        decoder->output.reset();
        output->isLastFrame = true;
        return 0;
    }
    const auto target = *decoder->output;
    EmitFrame(*decoder, target, output);
    decoder->output.reset();
    return 0;
}

int APS5_VABI sceVdecswFinalizeDecodeSequence(void* handle) {
    const auto decoder = FindDecoder(handle);
    if (decoder == nullptr) return VDECSW_ERROR_DECODER_INSTANCE;
    std::lock_guard guard(decoder->mutex);
    if (!decoder->finalized) {
        Send(*decoder, nullptr);
        decoder->finalized = true;
    }
    return 0;
}

int APS5_VABI sceVdecswGetAvcPictureInfo(const OutputInfo* output, AvcPictureInfo* first, AvcPictureInfo* second) {
    if (output == nullptr || first == nullptr) return VDECSW_ERROR_ARGUMENT_POINTER;
    if (output->thisSize != sizeof(OutputInfo) && output->thisSize != OutputInfoV1Size) return VDECSW_ERROR_STRUCT_SIZE;
    if (first->thisSize != sizeof(AvcPictureInfo) || (second != nullptr && second->thisSize != sizeof(AvcPictureInfo))) return VDECSW_ERROR_STRUCT_SIZE;
    std::vector<std::shared_ptr<Decoder>> all;
    {
        std::lock_guard guard(registryMutex);
        for (const auto& [key, decoder] : decoders) all.push_back(decoder);
    }
    std::optional<Picture> picture;
    for (const auto& decoder : all) {
        std::lock_guard guard(decoder->mutex);
        const auto found = decoder->pictures.find(output->frameBuffer);
        if (found != decoder->pictures.end()) {
            picture = found->second;
            break;
        }
    }
    const auto thisSize = first->thisSize;
    std::memset(first, 0, sizeof(AvcPictureInfo));
    first->thisSize = thisSize;
    if (!picture) return 0;
    first->isValid = true;
    first->ptsData = picture->pts;
    first->dtsData = picture->dts;
    first->attachedData = picture->attached;
    first->idrPictureFlag = picture->idr ? 1 : 0;
    first->profileIdc = picture->profile;
    first->levelIdc = picture->level;
    const auto codedWidth = AlignUp(picture->width, 16);
    const auto codedHeight = AlignUp(picture->height, 16);
    first->picWidthInMbsMinus1 = codedWidth / 16 - 1;
    first->picHeightInMapUnitsMinus1 = codedHeight / 16 - 1;
    first->frameMbsOnlyFlag = 1;
    first->frameCroppingFlag = codedWidth != picture->width || codedHeight != picture->height ? 1 : 0;
    first->frameCropRightOffset = (codedWidth - picture->width) / 2;
    first->frameCropBottomOffset = (codedHeight - picture->height) / 2;
    return 0;
}

}

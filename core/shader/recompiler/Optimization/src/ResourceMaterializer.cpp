#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

namespace {

constexpr std::uint32_t NoRemap = std::numeric_limits<std::uint32_t>::max();

std::atomic<std::uint64_t> specializationNanoseconds{0};

bool MaterializeProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct DecodedImage {
    IrTextureNumericClass numericClass = IrTextureNumericClass::Unsupported;
    RdnaImageDimension dimension = RdnaImageDimension::Unknown;
    std::uint32_t mipCount = 1;
    IrBufferFormat conversionFormat = IrBufferFormat::Invalid;
    std::uint32_t shaderSwizzle = ShaderImageIdentitySwizzle;
    bool cube = false;
    bool fmask = false;
    bool depthBits = false;
    bool depthUnorm16 = false;
    IrBufferFormat packedFormat = IrBufferFormat::Invalid;
    bool srgbDecode = false;
};

ShaderBufferResource decodeBufferDescriptor(const DescriptorValue& value) {
    if (value.dwordCount != 4u) {
        throw std::runtime_error("buffer descriptor has an invalid width");
    }
    ShaderBufferResource result;
    for (std::uint32_t i = 0; i < 4u; i++) {
        result.fields[i] = value.dwords[i];
    }
    return result;
}

bool nullImageDescriptor(const DescriptorValue& descriptor) {
    return descriptor.dwords[0] == 0u && (descriptor.dwords[1] & 0xffu) == 0u;
}

ImageType rawImageType(const DescriptorValue& descriptor) {
    return static_cast<ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
}

IrBufferFormat rawImageFormat(const DescriptorValue& descriptor) {
    return static_cast<IrBufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
}

std::uint32_t descriptorImageSwizzle(const DescriptorValue& descriptor) {
    return descriptor.dwords[3] & 0xfffu;
}

bool comparesNatively(const DescriptorValue& descriptor) {
    const auto format = rawImageFormat(descriptor);
    return format == IrBufferFormat::Format32Float || format == IrBufferFormat::Format16UNorm || IsDepthBitsTexture(descriptor.dwords[1], descriptor.dwords[3]);
}

bool descriptorIsCube(const DescriptorValue& descriptor) {
    return rawImageType(descriptor) == ImageType::Cube;
}

RdnaImageDimension descriptorDimension(const DescriptorValue& descriptor, RdnaImageDimension requested) {
    const bool wantArray = requested == RdnaImageDimension::Dim1DArray || requested == RdnaImageDimension::Dim2DArray || requested == RdnaImageDimension::Dim2DMsaaArray;
    switch (rawImageType(descriptor)) {
        case ImageType::Color1D:
            return RdnaImageDimension::Dim1D;
        case ImageType::Color1DArray:
            return wantArray ? RdnaImageDimension::Dim1DArray : RdnaImageDimension::Dim1D;
        case ImageType::Color3D:
            return RdnaImageDimension::Dim3D;
        case ImageType::Cube:
            return RdnaImageDimension::Dim2DArray;
        case ImageType::Color2DArray:
            return wantArray ? RdnaImageDimension::Dim2DArray : RdnaImageDimension::Dim2D;
        case ImageType::Color2DMsaaArray:
            return wantArray ? RdnaImageDimension::Dim2DMsaaArray : RdnaImageDimension::Dim2DMsaa;
        case ImageType::Color2D:
            return RdnaImageDimension::Dim2D;
        case ImageType::Color2DMsaa:
            return RdnaImageDimension::Dim2DMsaa;
        default:
            throw std::runtime_error("image descriptor has an unsupported type");
    }
}

bool validImageDescriptor(const DescriptorValue& descriptor, bool r128) {
    const auto type = rawImageType(descriptor);
    const auto format = rawImageFormat(descriptor);
    if (type < ImageType::Color1D || format == IrBufferFormat::Invalid) {
        return false;
    }
    if (r128 && type != ImageType::Color1D && type != ImageType::Color2D && type != ImageType::Color2DMsaa) {
        return false;
    }
    if (type == ImageType::Color2DMsaa || type == ImageType::Color2DMsaaArray) {
        const auto baseLevel = (descriptor.dwords[3] >> 12u) & 0xfu;
        const auto fragments = (descriptor.dwords[3] >> 16u) & 0xfu;
        const auto maxMip = (descriptor.dwords[5] >> 4u) & 0xfu;
        return baseLevel == 0u && fragments >= 1u && fragments <= 3u && (r128 || maxMip == fragments);
    }
    return true;
}

std::uint32_t storageMipCount(const ImageResource& base, const DescriptorValue& descriptor) {
    if (base.mipMode != ImageMipMode::DynamicStorage || nullImageDescriptor(descriptor)) {
        return 1u;
    }
    const auto mipBase = (descriptor.dwords[3] >> 12u) & 0xfu;
    const auto mipLast = (descriptor.dwords[3] >> 16u) & 0xfu;
    return mipBase <= mipLast ? mipLast - mipBase + 1u : 0u;
}

using ImageTableAbi::PoisonReason;

struct ImageDecode {
    DecodedImage decoded;
    std::optional<PoisonReason> reason;
    std::string detail;
};

ImageDecode rejectImage(PoisonReason reason, std::string detail) {
    ImageDecode result;
    result.reason = reason;
    result.detail = std::move(detail);
    return result;
}

PoisonReason invalidImageReason(const DescriptorValue& descriptor) {
    const auto type = rawImageType(descriptor);
    if (type < ImageType::Color1D) {
        return PoisonReason::NotImage;
    }
    if (rawImageFormat(descriptor) == IrBufferFormat::Invalid) {
        return PoisonReason::InvalidFormat;
    }
    if (type == ImageType::Color2DMsaa || type == ImageType::Color2DMsaaArray) {
        return PoisonReason::Multisampled;
    }
    return PoisonReason::Dimension;
}

ImageDecode inspectImageDescriptor(const DescriptorValue& descriptor, const ImageResource& base, std::uint32_t srgbDecodeFormats) {
    ImageDecode result;
    auto& decoded = result.decoded;
    decoded.mipCount = storageMipCount(base, descriptor);
    if (decoded.mipCount == 0u) {
        return rejectImage(PoisonReason::InvalidFormat, "storage image descriptor has an invalid mip range");
    }
    if (nullImageDescriptor(descriptor)) {
        decoded.numericClass = base.atomic ? IrTextureNumericClass::Uint : IrTextureNumericClass::Float;
        decoded.dimension = RdnaImageDimension::Dim2D;
        decoded.cube = false;
        return result;
    }
    if (base.resourceClass == ImageResourceClass::None || (base.atomic && base.resourceClass != ImageResourceClass::Storage)) {
        throw std::runtime_error("image resource has an invalid class");
    }
    if (!validImageDescriptor(descriptor, base.r128)) {
        return rejectImage(invalidImageReason(descriptor), "image descriptor is invalid");
    }
    decoded.dimension = descriptorDimension(descriptor, base.dimension);
    decoded.cube = descriptorIsCube(descriptor);
    const auto format = rawImageFormat(descriptor);
    if (base.atomic64 && format != IrBufferFormat::Format32_32UInt && format != IrBufferFormat::Format32_32SInt && format != IrBufferFormat::Format32_32Float) {
        return rejectImage(PoisonReason::InvalidFormat, "64-bit atomic image descriptor uses an unsupported format " + std::to_string(static_cast<std::uint32_t>(format)));
    }
    if (base.atomic && !base.atomic64 && format != IrBufferFormat::Format32UInt && format != IrBufferFormat::Format32SInt && format != IrBufferFormat::Format32Float) {
        return rejectImage(PoisonReason::InvalidFormat, "atomic image descriptor uses an unsupported format " + std::to_string(static_cast<std::uint32_t>(format)));
    }
    const bool storage = base.resourceClass == ImageResourceClass::Storage;
    decoded.fmask = IsFmaskTextureFormat(format);
    if (decoded.fmask && (storage || base.depthCompare || base.tableView != NoTableView)) {
        return rejectImage(PoisonReason::Fmask, "FMASK requires a direct sampled image load");
    }
    if (base.packed) {
        if (base.tableView != NoTableView || (!storage && descriptorImageSwizzle(descriptor) != ShaderImageIdentitySwizzle)) {
            return rejectImage(PoisonReason::InvalidFormat, "packed image access requires a direct image, with identity swizzle when sampled");
        }
        decoded.packedFormat = format;
    }
    decoded.conversionFormat = RemapTextureFormat(format) != format ? format : IrBufferFormat::Invalid;
    decoded.srgbDecode = !storage && (srgbDecodeFormats & SrgbDecodeBit(format)) != 0u;
    if (storage || decoded.conversionFormat != IrBufferFormat::Invalid) {
        decoded.shaderSwizzle = descriptorImageSwizzle(descriptor);
    }
    const bool wideSint = format == IrBufferFormat::Format32SInt || format == IrBufferFormat::Format32_32SInt || format == IrBufferFormat::Format32_32_32_32SInt;
    const bool narrowSint = format == IrBufferFormat::Format16SInt || format == IrBufferFormat::Format8_8SInt || format == IrBufferFormat::Format16_16SInt || format == IrBufferFormat::Format8_8_8_8SInt || format == IrBufferFormat::Format16_16_16_16SInt;
    const bool rawSintStorage = storage && (wideSint || (narrowSint && !base.packed)) && base.written && !base.read && !base.atomic;
    decoded.numericClass = base.atomic ? IrTextureNumericClass::Uint : SampledTextureNumericClass(format);
    if (!storage && !base.depthCompare && IsDepthBitsTexture(descriptor.dwords[1], descriptor.dwords[3])) {
        decoded.depthBits = true;
        decoded.depthUnorm16 = DepthBitsTextureWidth(descriptor.dwords[1], descriptor.dwords[3]) == 16u;
        decoded.numericClass = IrTextureNumericClass::Float;
        decoded.shaderSwizzle = descriptorImageSwizzle(descriptor);
    }
    if (storage) {
        if ((!rawSintStorage && decoded.numericClass == IrTextureNumericClass::Sint) || decoded.numericClass == IrTextureNumericClass::Unsupported) {
            return rejectImage(PoisonReason::NumericClass, "storage image descriptor uses an unsupported format");
        }
        if (rawSintStorage) {
            decoded.numericClass = IrTextureNumericClass::Uint;
        }
        if ((rawSintStorage || (base.atomic && format == IrBufferFormat::Format32SInt)) && !base.packed) {
            decoded.conversionFormat = format;
        }
    } else if (decoded.numericClass == IrTextureNumericClass::Unsupported || (base.depthCompare && decoded.numericClass != IrTextureNumericClass::Float)) {
        return rejectImage(PoisonReason::NumericClass, "sampled image descriptor uses an unsupported format");
    }
    return result;
}

DecodedImage decodeImageDescriptor(const DescriptorValue& descriptor, const ImageResource& base, std::uint32_t srgbDecodeFormats) {
    auto result = inspectImageDescriptor(descriptor, base, srgbDecodeFormats);
    if (result.reason.has_value()) {
        throw std::runtime_error(result.detail);
    }
    return result.decoded;
}

bool requiresPointSampler(const ResourceSpecialization::Image& image) {
    return image.numericClass == IrTextureNumericClass::Uint || image.numericClass == IrTextureNumericClass::Sint || image.conversionFormat != IrBufferFormat::Invalid || image.depthBits;
}

std::uint32_t TableKeyScanLimit() {
    static const std::uint32_t limit = [] {
        const char* text = std::getenv("APS5_IMAGE_TABLE_KEY_SCAN");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 256u;
    }();
    return limit;
}

bool TableStrict() {
    static const bool strict = [] {
        const char* text = std::getenv("APS5_IMAGE_TABLE_STRICT");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return strict;
}

bool TableTraced() {
    static const bool traced = std::getenv("APS5_TRACE_IMAGE_TABLE") != nullptr;
    return traced;
}

constexpr std::size_t PoisonReasonCount = static_cast<std::size_t>(PoisonReason::DriverRejected) + 1u;

struct TableCounters {
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> views{0};
    std::atomic<std::uint64_t> keys{0};
    std::array<std::atomic<std::uint64_t>, 4> entries{};
    std::atomic<std::uint64_t> samplers{0};
    std::array<std::atomic<std::uint64_t>, PoisonReasonCount> poison{};
    std::atomic<std::uint64_t> narrowed{0};
    std::atomic<std::uint64_t> narrowSkipped{0};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::atomic<long long> lastReport{0};
};

TableCounters& tableCounters() {
    static TableCounters counters;
    return counters;
}

void reportTables() {
    if (!MaterializeProfiled()) return;
    auto& counters = tableCounters();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load(std::memory_order_relaxed);
    if (last == 0) {
        counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10'000'000'000ll || !counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const auto snapshots = counters.snapshots.exchange(0, std::memory_order_relaxed);
    if (snapshots == 0) return;
    std::string poison;
    for (std::size_t reason = 0; reason < PoisonReasonCount; reason++) {
        const auto count = counters.poison[reason].exchange(0, std::memory_order_relaxed);
        if (count == 0) continue;
        poison += " " + std::string(ImageTableAbi::PoisonReasonName(static_cast<PoisonReason>(reason))) + "=" + std::to_string(count);
    }
    std::fprintf(stderr, "[image-table] (10 s): %llu snapshots, %llu views, %llu keys, image entries float %llu uint %llu sint %llu, sampler entries %llu, poison:%s, narrowing %llu applied / %llu skipped, %.1f ms\n",
        static_cast<unsigned long long>(snapshots), static_cast<unsigned long long>(counters.views.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.keys.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(counters.entries[ImageTableAbi::FloatClass].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.entries[ImageTableAbi::UintClass].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.entries[ImageTableAbi::SintClass].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(counters.samplers.exchange(0, std::memory_order_relaxed)), poison.empty() ? " none" : poison.c_str(),
        static_cast<unsigned long long>(counters.narrowed.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.narrowSkipped.exchange(0, std::memory_order_relaxed)), static_cast<double>(counters.nanoseconds.exchange(0, std::memory_order_relaxed)) / 1e6);
}

std::uint32_t tableClass(IrTextureNumericClass numericClass) {
    switch (numericClass) {
        case IrTextureNumericClass::Float: return ImageTableAbi::FloatClass;
        case IrTextureNumericClass::Uint: return ImageTableAbi::UintClass;
        case IrTextureNumericClass::Sint: return ImageTableAbi::SintClass;
        case IrTextureNumericClass::Unsupported: break;
    }
    return 0u;
}

ImageResource tableViewTemplate(const TableView& view, std::uint32_t index) {
    ImageResource image;
    image.resourceClass = ImageResourceClass::Sampled;
    image.dimension = view.dimension;
    image.depthCompare = view.depthCompare;
    image.r128 = view.r128;
    image.read = true;
    image.tableView = index;
    return image;
}

std::optional<PoisonReason> classifyTableImage(const DescriptorValue& words, const ImageResource& view, std::uint32_t srgbDecodeFormats, DecodedImage& decoded) {
    if (!validImageDescriptor(words, view.r128)) {
        return invalidImageReason(words);
    }
    const auto inspected = inspectImageDescriptor(words, view, srgbDecodeFormats);
    if (inspected.reason.has_value()) {
        return inspected.reason;
    }
    decoded = inspected.decoded;
    if (decoded.cube) {
        return PoisonReason::Cube;
    }
    if (decoded.dimension != view.dimension) {
        return decoded.dimension == RdnaImageDimension::Dim2DMsaa || decoded.dimension == RdnaImageDimension::Dim2DMsaaArray ? PoisonReason::Multisampled : PoisonReason::Dimension;
    }
    if (decoded.fmask) {
        return PoisonReason::Fmask;
    }
    if (decoded.depthBits) {
        return PoisonReason::DepthBits;
    }
    if (decoded.conversionFormat != IrBufferFormat::Invalid || decoded.srgbDecode || decoded.packedFormat != IrBufferFormat::Invalid || decoded.shaderSwizzle != ShaderImageIdentitySwizzle || decoded.mipCount != 1u) {
        return PoisonReason::Conversion;
    }
    if (tableClass(decoded.numericClass) == 0u || (view.depthCompare && decoded.numericClass != IrTextureNumericClass::Float)) {
        return PoisonReason::NumericClass;
    }
    return std::nullopt;
}

enum class RecordState : std::uint8_t { OutsideDomain, Read, Unmapped };

struct ColumnRecords {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t records = 0;
    std::uint32_t keys = 0;
    bool narrowed = false;
    std::vector<RecordState> states;
    std::vector<DescriptorValue> words;
};

struct TableTrace {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t records = 0;
    std::uint32_t keys = 0;
    std::uint32_t entries = 0;
    std::uint32_t poison = 0;
    bool narrowed = false;

    bool operator==(const TableTrace& other) const = default;
};

void traceTable(const IrResourcePlan& plan, std::uint32_t viewIndex, const TableColumn& column, const TableTrace& trace) {
    static std::mutex mutex;
    static std::map<std::pair<std::uint64_t, std::uint32_t>, TableTrace> seen;
    std::lock_guard lock(mutex);
    auto& last = seen[{plan.shaderHash, viewIndex}];
    if (last == trace) return;
    last = trace;
    const auto& view = plan.info.tableViews[viewIndex];
    std::fprintf(stderr, "[image-table] shader 0x%llx view %u (%s): base 0x%llx size 0x%llx stride 0x%x addend 0x%x offset 0x%x dwords %u, %u records, %u keys%s, %u entries, %u poisoned\n", static_cast<unsigned long long>(plan.shaderHash), viewIndex, view.sampler ? "sampler" : "image", static_cast<unsigned long long>(trace.base), static_cast<unsigned long long>(trace.size), column.stride, column.addend, column.offset, column.dwordCount, trace.records, trace.keys, trace.narrowed ? " (narrowed)" : "", trace.entries, trace.poison);
}

class TableSnapshotter {
public:
    TableSnapshotter(const IrResourcePlan& plan, const SrtRuntime& runtime, SrtWalker& walker, const std::vector<std::uint8_t>& activeSources, ImageTableSnapshot& tables) : plan(plan), runtime(runtime), walker(walker), activeSources(activeSources), tables(tables) {}

    void Run() {
        const auto& views = plan.info.tableViews;
        tables = ImageTableSnapshot{};
        if (views.empty()) {
            return;
        }
        if (runtime.readMemory == nullptr) {
            throw std::runtime_error("image table snapshot requires runtime memory access");
        }
        const auto started = std::chrono::steady_clock::now();
        tables.shader = plan.shaderHash;
        tables.map.assign(ImageTableAbi::HeaderWords + ImageTableAbi::ViewWords * views.size(), 0u);
        tables.poison.push_back({0u, {}, 0u, PoisonReason::OutsideSnapshot});
        for (std::uint32_t view = 0; view < views.size(); view++) {
            if (!views[view].sampler) SnapshotView(view);
        }
        for (std::uint32_t view = 0; view < views.size(); view++) {
            if (views[view].sampler) SnapshotView(view);
        }
        tables.map[0] = ImageTableAbi::Version;
        tables.map[1] = static_cast<std::uint32_t>(views.size());
        tables.map[2] = static_cast<std::uint32_t>(tables.poison.size());
        auto& counters = tableCounters();
        counters.snapshots.fetch_add(1, std::memory_order_relaxed);
        counters.views.fetch_add(views.size(), std::memory_order_relaxed);
        counters.samplers.fetch_add(tables.samplers.size(), std::memory_order_relaxed);
        for (const auto& image : tables.images) counters.entries[tableClass(image.numericClass)].fetch_add(1, std::memory_order_relaxed);
        for (std::size_t index = 1; index < tables.poison.size(); index++) counters.poison[static_cast<std::size_t>(tables.poison[index].reason)].fetch_add(1, std::memory_order_relaxed);
        counters.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
        if (TableStrict() && tables.poison.size() > 1u) {
            const auto& first = tables.poison[1];
            throw std::runtime_error("image table: view " + std::to_string(first.view) + " holds an entry at 0x" + hex(first.address) + " that cannot be bound (" + std::string(ImageTableAbi::PoisonReasonName(first.reason)) + "), and APS5_IMAGE_TABLE_STRICT is set");
        }
    }

private:
    static std::string hex(std::uint64_t value) {
        char text[24];
        std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
        return text;
    }

    bool Accessible(std::uint64_t address, std::uint64_t bytes) const {
        return runtime.accessible == nullptr || runtime.accessible(runtime.userContext, address, bytes);
    }

    std::uint32_t Read(std::uint64_t address) const {
        std::uint32_t word = 0;
        if (!runtime.readMemory(runtime.userContext, address, &word)) {
            throw std::runtime_error("image table: failed to read guest memory at 0x" + hex(address));
        }
        if (runtime.readTrace != nullptr) runtime.readTrace->otherReads.push_back(address);
        return word;
    }

    ShaderBufferResource EvaluateBuffer(std::uint32_t source) const {
        DescriptorValue value;
        walker.EvaluateDescriptorSource(plan, source, runtime, value);
        return decodeBufferDescriptor(value);
    }

    void Narrow(const TableColumn& column, ColumnRecords& records) {
        const auto& domain = *column.keyDomain;
        const auto keyBuffer = EvaluateBuffer(domain.source);
        if (keyBuffer.Type() != 0u) {
            tableCounters().narrowSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto base = keyBuffer.Base48();
        const auto size = keyBuffer.GetSize();
        const std::uint64_t first = domain.offset & ~3u;
        const std::uint64_t positions = size > first ? (size - first + domain.alignment - 1u) / domain.alignment : 0u;
        const auto skip = [&] {
            tableCounters().narrowSkipped.fetch_add(1, std::memory_order_relaxed);
        };
        if (positions > TableKeyScanLimit() || first > 0xffffffffull) {
            skip();
            return;
        }
        if (positions != 0u && runtime.pendingWrite != nullptr && runtime.pendingWrite(runtime.userContext, base + first, size - first)) {
            skip();
            return;
        }
        std::vector<std::uint32_t> values{0u};
        for (std::uint64_t index = 0; index < positions; index++) {
            const auto address = base + first + index * domain.alignment;
            if (!Accessible(address, sizeof(std::uint32_t))) {
                skip();
                return;
            }
            values.push_back(Read(address));
        }
        records.states.assign(records.keys, RecordState::OutsideDomain);
        for (const auto value : values) {
            const auto relative = value * column.stride;
            if (relative % column.stride != 0u) continue;
            const auto record = relative / column.stride;
            if (record < records.keys) records.states[record] = RecordState::Read;
        }
        records.narrowed = true;
        if (positions != 0u) tables.ranges.push_back({base, size});
        tableCounters().narrowed.fetch_add(1, std::memory_order_relaxed);
    }

    ColumnRecords& Column(std::uint32_t source) {
        if (const auto found = columns.find(source); found != columns.end()) {
            return found->second;
        }
        auto& records = columns[source];
        const auto& column = *plan.descriptorSources.at(source).tableColumn;
        const bool active = source >= activeSources.size() || activeSources[source] != 0u;
        if (!active) {
            return records;
        }
        if (column.address) {
            DescriptorValue pointer;
            walker.EvaluateDescriptorSource(plan, column.heapSource, runtime, pointer);
            records.base = (static_cast<std::uint64_t>(pointer.dwords[1]) << 32u) | pointer.dwords[0];
            records.records = ImageTableAbi::ScalarAddressRecords(column.stride);
        } else {
            const auto heap = EvaluateBuffer(column.heapSource);
            if (heap.Type() != 0u) {
                throw std::runtime_error("image table: the descriptor table V# has type " + std::to_string(heap.Type()) + ", not a buffer");
            }
            records.base = heap.Base48();
            records.size = heap.GetSize();
            records.records = ImageTableAbi::ScalarBufferRecords(column.addend, column.stride, column.offset, records.size);
            tables.ranges.push_back({records.base, records.size});
        }
        records.records = std::min(records.records, ImageTableAbi::KeyRecords(column.maxKey, column.stride));
        records.keys = std::min(records.records, ImageTableAbi::MaxKeys);
        records.states.assign(records.keys, RecordState::Read);
        if (column.keyDomain.has_value() && records.keys != 0u) {
            Narrow(column, records);
        }
        records.words.assign(records.keys, DescriptorValue{});
        std::uint64_t low = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t high = 0;
        for (std::uint32_t record = 0; record < records.keys; record++) {
            if (records.states[record] != RecordState::Read) continue;
            auto& value = records.words[record];
            value.dwordCount = column.sampler ? 4u : 8u;
            const auto offset = column.addend + record * column.stride;
            for (std::uint32_t dword = 0; dword < column.dwordCount; dword++) {
                std::optional<std::uint64_t> address;
                if (column.address) {
                    address = ImageTableAbi::ScalarAddressDword(records.base, offset, column.offset + dword * 4u);
                    if (address.has_value() && (*address & 3u) != 0u) address.reset();
                } else if (const auto position = ImageTableAbi::ScalarBufferDword(offset, column.offset + dword * 4u, records.size); position.has_value()) {
                    address = records.base + *position;
                } else {
                    continue;
                }
                if (!address.has_value() || !Accessible(*address, sizeof(std::uint32_t))) {
                    records.states[record] = RecordState::Unmapped;
                    value = DescriptorValue{};
                    break;
                }
                value.dwords[dword] = Read(*address);
                low = std::min(low, *address);
                high = std::max(high, *address + sizeof(std::uint32_t));
            }
        }
        if (column.address && low < high) tables.ranges.push_back({low, high - low});
        return records;
    }

    std::uint64_t RecordAddress(const TableColumn& column, const ColumnRecords& records, std::uint32_t record) const {
        const auto offset = column.addend + record * column.stride;
        if (column.address) return ImageTableAbi::ScalarAddressDword(records.base, offset, column.offset).value_or(0u);
        return records.base + ((offset + column.offset) & ~3u);
    }

    using WordsKey = std::pair<std::array<std::uint32_t, 8>, std::uint32_t>;

    static WordsKey Words(const DescriptorValue& value) {
        return {value.dwords, value.dwordCount};
    }

    std::uint32_t Poison(std::uint32_t view, std::uint64_t address, const DescriptorValue& words, PoisonReason reason) {
        const auto [found, inserted] = poisonIndex.try_emplace({view, reason, Words(words)}, static_cast<std::uint32_t>(tables.poison.size()));
        if (inserted) {
            tables.poison.push_back({address, words, view, reason});
        }
        return ImageTableAbi::PoisonCode(found->second);
    }

    std::uint32_t ImageEntry(const DescriptorValue& words, const TableView& view, IrTextureNumericClass numericClass) {
        const auto key = std::make_tuple(Words(words), view.dimension, numericClass, view.depthCompare);
        if (const auto found = imageIndex.find(key); found != imageIndex.end()) {
            return found->second;
        }
        if (tables.images.size() >= ImageTableAbi::ImageBudget) {
            return NoRemap;
        }
        const auto entry = static_cast<std::uint32_t>(tables.images.size());
        imageIndex.emplace(key, entry);
        tables.images.push_back({words, view.dimension, numericClass, view.depthCompare});
        return entry;
    }

    std::uint32_t SamplerEntry(const DescriptorValue& words, bool compare) {
        const auto key = std::make_pair(Words(words), compare);
        if (const auto found = samplerIndex.find(key); found != samplerIndex.end()) {
            return found->second;
        }
        if (tables.samplers.size() >= ImageTableAbi::SamplerBudget) {
            return NoRemap;
        }
        const auto entry = static_cast<std::uint32_t>(tables.samplers.size());
        samplerIndex.emplace(key, entry);
        tables.samplers.push_back({words, compare, false});
        return entry;
    }

    void SnapshotView(std::uint32_t viewIndex) {
        const auto& view = plan.info.tableViews[viewIndex];
        const auto& column = *plan.descriptorSources.at(view.source).tableColumn;
        const auto& records = Column(view.source);
        const auto header = ImageTableAbi::HeaderWords + ImageTableAbi::ViewWords * viewIndex;
        const auto mapStart = static_cast<std::uint32_t>(tables.map.size());
        std::uint32_t zeroCode = ImageTableAbi::NullCode;
        if (view.sampler) {
            DescriptorValue zero;
            zero.dwordCount = 4u;
            const auto entry = SamplerEntry(zero, view.depthCompare);
            if (entry == NoRemap) {
                throw std::runtime_error("image table: the sampler budget leaves no room for the all-zero S#");
            }
            zeroCode = ImageTableAbi::SamplerCode(entry);
        }
        const auto imageView = tableViewTemplate(view, viewIndex);
        TableTrace trace{records.base, records.size, records.records, records.keys, 0u, 0u, records.narrowed};
        for (std::uint32_t record = 0; record < records.keys; record++) {
            const auto address = RecordAddress(column, records, record);
            std::uint32_t code = ImageTableAbi::NullCode;
            if (records.states[record] == RecordState::OutsideDomain) {
                code = Poison(viewIndex, 0u, {}, PoisonReason::OutsideDomain);
            } else if (records.states[record] == RecordState::Unmapped) {
                code = Poison(viewIndex, address, {}, PoisonReason::Unmapped);
            } else {
                const auto& words = records.words[record];
                if (view.sampler && ((words.dwords[0] >> 29u) & 3u) != 0u) {
                    code = Poison(viewIndex, address, words, PoisonReason::Reduction);
                } else if (view.sampler) {
                    const auto entry = SamplerEntry(words, view.depthCompare);
                    code = entry == NoRemap ? Poison(viewIndex, address, words, PoisonReason::Budget) : ImageTableAbi::SamplerCode(entry);
                } else if (std::all_of(words.dwords.begin(), words.dwords.end(), [](std::uint32_t word) { return word == 0u; })) {
                    code = ImageTableAbi::NullCode;
                } else {
                    DecodedImage decoded;
                    const auto reason = classifyTableImage(words, imageView, plan.srgbDecodeFormats, decoded);
                    if (reason.has_value()) {
                        code = Poison(viewIndex, address, words, *reason);
                    } else if (view.depthCompare && !comparesNatively(words)) {
                        throw std::runtime_error("image table: view " + std::to_string(viewIndex) + " holds a color texture at 0x" + hex(address) + " (format " + std::to_string(static_cast<std::uint32_t>(rawImageFormat(words))) + ") under comparison sampling, which is emulated only for a T# outside a table");
                    } else {
                        const auto entry = ImageEntry(words, view, decoded.numericClass);
                        code = entry == NoRemap ? Poison(viewIndex, address, words, PoisonReason::Budget) : ImageTableAbi::ImageCode(tableClass(decoded.numericClass), entry);
                    }
                }
            }
            if ((code & ImageTableAbi::PoisonFlag) != 0u) trace.poison++;
            else if (code != ImageTableAbi::NullCode) trace.entries++;
            tables.map.push_back(code);
        }
        tables.map[header + ImageTableAbi::ViewMapStart] = mapStart;
        tables.map[header + ImageTableAbi::ViewKeyCount] = records.keys;
        tables.map[header + ImageTableAbi::ViewSizeLow] = static_cast<std::uint32_t>(records.size);
        tables.map[header + ImageTableAbi::ViewSizeHigh] = static_cast<std::uint32_t>(records.size >> 32u);
        tables.map[header + ImageTableAbi::ViewBaseLow] = static_cast<std::uint32_t>(records.base);
        tables.map[header + ImageTableAbi::ViewBaseHigh] = static_cast<std::uint32_t>(records.base >> 32u);
        tables.map[header + ImageTableAbi::ViewZeroCode] = zeroCode;
        tableCounters().keys.fetch_add(records.keys, std::memory_order_relaxed);
        if (TableTraced()) traceTable(plan, viewIndex, column, trace);
    }

    const IrResourcePlan& plan;
    const SrtRuntime& runtime;
    SrtWalker& walker;
    const std::vector<std::uint8_t>& activeSources;
    ImageTableSnapshot& tables;
    std::map<std::uint32_t, ColumnRecords> columns;
    std::map<std::tuple<std::uint32_t, PoisonReason, WordsKey>, std::uint32_t> poisonIndex;
    std::map<std::tuple<WordsKey, RdnaImageDimension, IrTextureNumericClass, bool>, std::uint32_t> imageIndex;
    std::map<std::pair<WordsKey, bool>, std::uint32_t> samplerIndex;
};

void materializeSnapshot(const IrResourcePlan& plan, const SrtRuntime& runtime, SrtWalker& walker, ResourceSnapshot& snapshot, std::vector<std::uint8_t>& activeSources) {
    snapshot = ResourceSnapshot{};
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        const auto words = plan.uniformFill.fill.words;
        if (words == 0u || words > plan.uniformFill.values.size()) {
            throw std::runtime_error("uniform fill plan has an invalid word count");
        }
        std::array<std::uint32_t, 4> stored{};
        walker.EvaluateUniformValues(plan, std::span(plan.uniformFill.values).first(words), runtime, std::span(stored).first(words));
        for (std::uint32_t i = 1; i < words; i++) {
            if (stored[i] != stored[0]) {
                throw std::runtime_error("uniform fill values diverge at runtime");
            }
        }
        snapshot.uniformFill = plan.uniformFill.fill;
        snapshot.uniformFill.value = stored[0];
    }
    if (runtime.userData.size() < plan.userDataCount) {
        throw std::runtime_error("runtime user data is smaller than the shader user data count");
    }
    snapshot.userData.assign(runtime.userData.begin(), runtime.userData.begin() + plan.userDataCount);

    std::vector<DescriptorValue> values;
    walker.EvaluateRuntimeSources(plan, plan.materializationSources, runtime, values, snapshot.flattenedSrt, plan.cleanFlatSlots, activeSources);

    std::size_t cursor = 0;
    if (values.size() < plan.info.buffers.size()) {
        throw std::runtime_error("materialization sources are missing buffer descriptors");
    }
    snapshot.buffers.assign(values.begin(), values.begin() + plan.info.buffers.size());
    cursor += plan.info.buffers.size();

    snapshot.images.resize(plan.info.images.size());
    for (std::uint32_t i = 0; i < plan.info.images.size(); i++) {
        const auto& image = plan.info.images[i];
        if (image.source >= plan.descriptorSources.size()) {
            throw std::runtime_error("image resource references an unknown descriptor source");
        }
        if (image.tableView != NoTableView) {
            snapshot.images[i].dwordCount = 8u;
            continue;
        }
        if (cursor >= values.size()) {
            throw std::runtime_error("materialization sources are missing image descriptors");
        }
        auto descriptor = values[cursor];
        cursor++;
        if (descriptor.dwordCount != 8u) {
            throw std::runtime_error("image descriptor has an invalid width");
        }
        if (!validImageDescriptor(descriptor, image.r128) && !nullImageDescriptor(descriptor)) {
            descriptor.dwords.fill(0u);
        }
        snapshot.images[i] = descriptor;
    }

    snapshot.samplers.resize(plan.info.samplers.size());
    for (std::uint32_t i = 0; i < plan.info.samplers.size(); i++) {
        if (plan.info.samplers[i].tableView != NoTableView) {
            snapshot.samplers[i].dwordCount = 4u;
            continue;
        }
        if (cursor >= values.size()) {
            throw std::runtime_error("materialization sources are missing sampler descriptors");
        }
        snapshot.samplers[i] = values[cursor];
        cursor++;
    }
}

std::uint32_t colorCompareReference(IrBufferFormat format) {
    switch (format) {
    case IrBufferFormat::Format8UNorm: case IrBufferFormat::Format8_8UNorm: case IrBufferFormat::Format16_16UNorm:
    case IrBufferFormat::Format11_11_10UNorm: case IrBufferFormat::Format10_11_11UNorm: case IrBufferFormat::Format2_10_10_10UNorm:
    case IrBufferFormat::Format10_10_10_2UNorm: case IrBufferFormat::Format8_8_8_8UNorm: case IrBufferFormat::Format16_16_16_16UNorm:
        return EmulatedCompare::ReferenceUnorm;
    case IrBufferFormat::Format8SNorm: case IrBufferFormat::Format16SNorm: case IrBufferFormat::Format8_8SNorm: case IrBufferFormat::Format16_16SNorm:
    case IrBufferFormat::Format11_11_10SNorm: case IrBufferFormat::Format10_11_11SNorm: case IrBufferFormat::Format2_10_10_10SNorm:
    case IrBufferFormat::Format10_10_10_2SNorm: case IrBufferFormat::Format8_8_8_8SNorm: case IrBufferFormat::Format16_16_16_16SNorm:
        return EmulatedCompare::ReferenceSnorm;
    case IrBufferFormat::Format16Float: case IrBufferFormat::Format16_16Float: case IrBufferFormat::Format11_11_10Float:
    case IrBufferFormat::Format10_11_11Float: case IrBufferFormat::Format32_32Float: case IrBufferFormat::Format16_16_16_16Float:
        return EmulatedCompare::ReferenceFloat;
    default:
        throw std::runtime_error("comparison sampling of a color texture is implemented only for float, unorm and snorm formats (format " + std::to_string(static_cast<std::uint32_t>(format)) + ")");
    }
}

std::uint32_t emulatedCompareState(const IrResourcePlan& plan, const ResourceSnapshot& snapshot, std::uint32_t index) {
    const auto& image = plan.info.images[index];
    const auto& descriptor = snapshot.images[index];
    if (!image.depthCompare || descriptor.dwordCount != 8u || nullImageDescriptor(descriptor)) return 0u;
    if (comparesNatively(descriptor)) return 0u;
    const auto format = rawImageFormat(descriptor);
    const auto reference = colorCompareReference(format);
    const auto type = rawImageType(descriptor);
    if (type != ImageType::Color2D && type != ImageType::Color2DArray) throw std::runtime_error("comparison sampling of a color texture is implemented only for 2D and 2D array views");
    if ((descriptorImageSwizzle(descriptor) & 0x7u) != 4u) throw std::runtime_error("comparison sampling of a color texture is implemented only when the view's X channel is red");
    std::optional<std::uint32_t> samplerState;
    for (const auto& pair : plan.info.sampledPairs) {
        if (pair.image != index) continue;
        if (pair.sampler >= snapshot.samplers.size() || snapshot.samplers[pair.sampler].dwordCount != 4u) throw std::runtime_error("comparison sampling of a color texture has no sampler descriptor");
        if (plan.info.samplers[pair.sampler].tableView != NoTableView) throw std::runtime_error("comparison sampling of a color texture through a sampler table is not implemented");
        const auto& words = snapshot.samplers[pair.sampler].dwords;
        const auto clampX = words[0] & 0x7u;
        const auto clampY = (words[0] >> 3u) & 0x7u;
        const auto function = (words[0] >> 12u) & 0x7u;
        const bool unnormalized = ((words[0] >> 15u) & 0x1u) != 0u;
        if (((words[0] >> 29u) & 0x3u) != 0u) throw std::runtime_error("comparison sampling of a color texture through a min or max reduction sampler is not implemented");
        const auto magFilter = (words[2] >> 20u) & 0x3u;
        const auto minFilter = (words[2] >> 22u) & 0x3u;
        const auto addressMode = [](std::uint32_t clamp) {
            if (clamp == 0u) return EmulatedCompare::AddressWrap;
            if (clamp == 2u) return EmulatedCompare::AddressEdge;
            if (clamp == 6u) return EmulatedCompare::AddressBorder;
            throw std::runtime_error("comparison sampling of a color texture is implemented only with wrap, clamp-to-edge or clamp-to-border addressing");
        };
        const auto addressX = addressMode(clampX);
        const auto addressY = addressMode(clampY);
        const auto borderType = (words[3] >> 30u) & 0x3u;
        const bool border = addressX == EmulatedCompare::AddressBorder || addressY == EmulatedCompare::AddressBorder;
        if (border && borderType == 3u) throw std::runtime_error("comparison sampling of a color texture with a border color table is not implemented");
        if (magFilter != minFilter || magFilter > 1u) throw std::runtime_error("comparison sampling of a color texture is implemented only with equal point or bilinear minification and magnification filters");
        if (unnormalized) throw std::runtime_error("comparison sampling of a color texture does not implement unnormalized coordinates");
        const auto state = EmulatedCompare::Enabled | (function << EmulatedCompare::FunctionShift) | (magFilter == 1u ? EmulatedCompare::Linear : 0u)
            | (addressX << EmulatedCompare::ClampXShift) | (addressY << EmulatedCompare::ClampYShift) | (border && borderType == 2u ? EmulatedCompare::BorderWhite : 0u);
        if (samplerState.has_value() && *samplerState != state) throw std::runtime_error("comparison sampling of a color texture through samplers that disagree is not implemented");
        samplerState = state;
    }
    if (!samplerState.has_value()) throw std::runtime_error("comparison sampling of a color texture has no paired sampler");
    const bool singleLevel = ((descriptor.dwords[3] >> 12u) & 0xfu) == ((descriptor.dwords[3] >> 16u) & 0xfu);
    return *samplerState | (reference << EmulatedCompare::ReferenceShift) | (singleLevel ? EmulatedCompare::SingleLevel : 0u);
}

void buildResourceSpecialization(const IrResourcePlan& plan, ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
    ResourceSpecialization result;
    result.buffers.reserve(plan.info.buffers.size());
    for (std::uint32_t i = 0; i < plan.info.buffers.size(); i++) {
        const ShaderBufferResource decoded = decodeBufferDescriptor(snapshot.buffers[i]);
        if (decoded.Type() != 0u) {
            throw std::runtime_error("buffer descriptor uses an unsupported type");
        }
        auto packedStride = decoded.PackedStride();
        const auto stride = packedStride & 0x3fffu;
        const bool swizzleActive = stride != 0u && ((packedStride >> 14u) & 1u) != 0u;
        if (stride == 0u) {
            packedStride &= ~((1u << 14u) | (3u << 16u));
        } else if (!swizzleActive) {
            packedStride &= ~(3u << 16u);
        }
        const auto& buffer = plan.info.buffers[i];
        ResourceSpecialization::Buffer entry;
        entry.packedStride = packedStride;
        entry.descriptorFormat = buffer.formatted ? decoded.Format() : IrBufferFormat::Invalid;
        entry.descriptorSwizzle = buffer.formatted ? decoded.DstSelXYZW() : DstSel(4, 5, 6, 7);
        entry.empty = decoded.GetSize() == 0u || decoded.Base48() == 0u;
        result.buffers.push_back(entry);
    }

    result.images.reserve(plan.info.images.size());
    for (std::uint32_t i = 0; i < plan.info.images.size(); i++) {
        const auto& image = plan.info.images[i];
        ResourceSpecialization::Image entry;
        if (image.tableView != NoTableView) {
            entry.dimension = image.dimension;
            result.images.push_back(entry);
            continue;
        }
        const DecodedImage decoded = decodeImageDescriptor(snapshot.images[i], image, plan.srgbDecodeFormats);
        if (decoded.fmask && std::any_of(plan.info.sampledPairs.begin(), plan.info.sampledPairs.end(), [i](const SampledResourcePair& pair) { return pair.image == i; })) {
            throw std::runtime_error("FMASK requires a direct image load");
        }
        entry.numericClass = decoded.numericClass;
        entry.dimension = decoded.dimension;
        entry.mipCount = decoded.mipCount;
        entry.conversionFormat = decoded.conversionFormat;
        entry.shaderSwizzle = decoded.shaderSwizzle;
        entry.cube = decoded.cube;
        entry.fmask = decoded.fmask;
        entry.depthBits = decoded.depthBits;
        entry.depthUnorm16 = decoded.depthUnorm16;
        entry.packedFormat = decoded.packedFormat;
        entry.emulatedCompare = emulatedCompareState(plan, snapshot, i);
        entry.srgbDecode = decoded.srgbDecode;
        result.images.push_back(entry);
    }

    const auto& views = plan.info.tableViews;
    auto& tables = snapshot.tables;
    result.tableViewClasses.assign(views.size(), 0u);
    for (std::uint32_t view = 0; view < views.size(); view++) {
        if (views[view].sampler) {
            result.tableViewClasses[view] = static_cast<std::uint8_t>(ImageTableAbi::SamplerUsedBit);
            continue;
        }
        const auto header = ImageTableAbi::HeaderWords + ImageTableAbi::ViewWords * view;
        const auto start = tables.map.at(header + ImageTableAbi::ViewMapStart);
        const auto keys = tables.map.at(header + ImageTableAbi::ViewKeyCount);
        std::uint32_t classes = 0;
        for (std::uint32_t key = 0; key < keys; key++) {
            const auto code = tables.map.at(start + key);
            if ((code & ImageTableAbi::PoisonFlag) != 0u || code == ImageTableAbi::NullCode) continue;
            const auto numericClass = (code >> ImageTableAbi::ClassShift) & ImageTableAbi::ClassMask;
            classes |= 1u << (numericClass - 1u);
        }
        result.tableViewClasses[view] = static_cast<std::uint8_t>(classes);
    }
    for (const auto& pair : plan.info.sampledPairs) {
        const auto samplerView = plan.info.samplers.at(pair.sampler).tableView;
        if (samplerView == NoTableView) continue;
        const auto imageView = plan.info.images.at(pair.image).tableView;
        const bool point = imageView != NoTableView ? (result.tableViewClasses.at(imageView) & (ImageTableAbi::UintBit | ImageTableAbi::SintBit)) != 0u : requiresPointSampler(result.images.at(pair.image));
        if (point) result.tableViewClasses.at(samplerView) |= static_cast<std::uint8_t>(ImageTableAbi::SamplerPointBit);
    }
    for (std::uint32_t view = 0; view < views.size(); view++) {
        if (!views[view].sampler || (result.tableViewClasses[view] & ImageTableAbi::SamplerPointBit) == 0u) continue;
        const auto header = ImageTableAbi::HeaderWords + ImageTableAbi::ViewWords * view;
        const auto start = tables.map.at(header + ImageTableAbi::ViewMapStart);
        const auto keys = tables.map.at(header + ImageTableAbi::ViewKeyCount);
        const auto mark = [&](std::uint32_t code) {
            if ((code & ImageTableAbi::PoisonFlag) != 0u || (code & ImageTableAbi::SamplerFlag) == 0u) return;
            tables.samplers.at(code & ImageTableAbi::ElementMask).point = true;
        };
        mark(tables.map.at(header + ImageTableAbi::ViewZeroCode));
        for (std::uint32_t key = 0; key < keys; key++) mark(tables.map.at(start + key));
    }

    result.boundDescriptors.clear();
    result.boundDescriptors.reserve(result.buffers.size() + result.images.size());
    for (std::uint32_t index = 0; index < result.buffers.size(); index++) {
        result.boundDescriptors.push_back(index);
    }
    for (std::uint32_t index = 0; index < result.images.size(); index++) {
        result.boundDescriptors.push_back(index);
    }
    specialization = std::move(result);
}

}

void ResourceMaterializer::Apply(IrProgram& program, const ResourceSpecialization& specialization) const {
    IrResourcePlan& resources = program.Resources();
    if (!resources.resourceTrackingComplete) {
        throw std::runtime_error("ResourceMaterializer::Apply requires a completed resource plan");
    }
    if (resources.info.buffers.size() != specialization.buffers.size()) {
        throw std::runtime_error("ResourceMaterializer::Apply buffer count mismatch");
    }
    if (resources.info.images.size() != specialization.images.size()) {
        throw std::runtime_error("ResourceMaterializer::Apply image count mismatch");
    }
    if (resources.info.tableViews.size() != specialization.tableViewClasses.size()) {
        throw std::runtime_error("ResourceMaterializer::Apply table view count mismatch");
    }

    auto buffers = resources.info.buffers;
    for (std::uint32_t i = 0; i < buffers.size(); i++) {
        buffers[i].packedStride = specialization.buffers[i].packedStride;
        buffers[i].descriptorFormat = specialization.buffers[i].descriptorFormat;
        buffers[i].descriptorSwizzle = specialization.buffers[i].descriptorSwizzle;
        buffers[i].empty = specialization.buffers[i].empty;
    }

    auto tableViews = resources.info.tableViews;
    for (std::uint32_t view = 0; view < tableViews.size(); view++) {
        tableViews[view].classes = specialization.tableViewClasses[view];
    }
    const auto viewClasses = [&](std::uint32_t view) -> std::uint32_t {
        return view < tableViews.size() ? tableViews[view].classes : 0u;
    };

    auto images = resources.info.images;
    for (std::uint32_t index = 0; index < specialization.images.size(); index++) {
        const auto& source = specialization.images[index];
        auto& image = images[index];
        image.numericClass = source.numericClass;
        image.dimension = source.dimension;
        image.mipCount = source.mipCount;
        image.conversionFormat = source.conversionFormat;
        image.shaderSwizzle = source.shaderSwizzle;
        image.cube = source.cube;
        image.depthBits = source.depthBits;
        image.depthUnorm16 = source.depthUnorm16;
        image.packedFormat = source.packedFormat;
        image.emulatedCompare = source.emulatedCompare;
        image.srgbDecode = source.srgbDecode;
        if ((source.emulatedCompare & EmulatedCompare::Enabled) != 0u) image.depthCompare = false;
    }

    const auto tableImage = [&](std::uint32_t image) {
        return resources.info.images[image].tableView != NoTableView;
    };
    const auto tableSampler = [&](std::uint32_t sampler) {
        return resources.info.samplers[sampler].tableView != NoTableView;
    };
    const auto directPoint = [&](std::uint32_t image) {
        return !tableImage(image) && requiresPointSampler(specialization.images[image]);
    };
    std::vector<std::uint32_t> pointSampler(resources.info.samplers.size(), NoRemap);
    std::uint32_t samplerCount = static_cast<std::uint32_t>(resources.info.samplers.size());
    std::vector<std::uint8_t> samplerUsage(resources.info.samplers.size(), 0u);
    for (const auto& pair : resources.info.sampledPairs) {
        if (pair.image >= images.size() || pair.sampler >= resources.info.samplers.size()) {
            throw std::runtime_error("ResourceMaterializer::Apply sampled pair is out of range");
        }
        if (tableSampler(pair.sampler)) {
            continue;
        }
        if (tableImage(pair.image)) {
            const auto classes = viewClasses(resources.info.images[pair.image].tableView);
            if ((classes & ImageTableAbi::FloatBit) != 0u) samplerUsage[pair.sampler] |= 1u;
            if ((classes & (ImageTableAbi::UintBit | ImageTableAbi::SintBit)) != 0u) samplerUsage[pair.sampler] |= 2u;
            continue;
        }
        samplerUsage[pair.sampler] |= requiresPointSampler(specialization.images[pair.image]) ? 2u : 1u;
    }
    for (std::uint32_t index = 0; index < resources.info.samplers.size(); index++) {
        if ((samplerUsage[index] & 2u) == 0u) {
            continue;
        }
        if ((samplerUsage[index] & 1u) == 0u) {
            pointSampler[index] = index;
        } else {
            if (samplerCount >= ShaderInfo::MaxSamplers) {
                throw std::runtime_error("ResourceMaterializer::Apply exceeds the sampler resource limit");
            }
            pointSampler[index] = samplerCount;
            samplerCount++;
        }
    }
    auto samplers = resources.info.samplers;
    auto sampledPairs = resources.info.sampledPairs;
    samplers.reserve(samplerCount);
    for (std::uint32_t index = 0; index < resources.info.samplers.size(); index++) {
        const auto target = pointSampler[index];
        if (target == NoRemap) {
            continue;
        }
        if (target == index) {
            samplers[index].forcePointFiltering = true;
        } else {
            if (target != samplers.size()) {
                throw std::runtime_error("ResourceMaterializer::Apply sampler plan is inconsistent");
            }
            auto sampler = samplers[index];
            sampler.forcePointFiltering = true;
            samplers.push_back(sampler);
        }
    }
    for (auto& pair : sampledPairs) {
        if (!tableSampler(pair.sampler)) {
            if (tableImage(pair.image)) {
                if ((viewClasses(images[pair.image].tableView) & (ImageTableAbi::UintBit | ImageTableAbi::SintBit)) != 0u) {
                    if (pointSampler[pair.sampler] == NoRemap) {
                        throw std::runtime_error("ResourceMaterializer::Apply missing point sampler for an image table pair");
                    }
                    pair.pointSampler = pointSampler[pair.sampler];
                }
            } else if (requiresPointSampler(specialization.images[pair.image])) {
                if (pointSampler[pair.sampler] == NoRemap) {
                    throw std::runtime_error("ResourceMaterializer::Apply missing point sampler for pair");
                }
                pair.sampler = pointSampler[pair.sampler];
            }
        }
        samplers[pair.sampler].depthCompare = samplers[pair.sampler].depthCompare || images[pair.image].depthCompare;
    }

    auto memoryInfo = resources.memoryInfo;
    std::vector<std::uint32_t> imageRemap(specialization.images.size());
    std::uint32_t remapCount = 0;
    for (std::uint32_t i = 0; i < specialization.images.size(); i++) {
        imageRemap[i] = specialization.images[i].fmask ? NoRemap : remapCount;
        if (!specialization.images[i].fmask) {
            remapCount++;
        }
    }

    IrBuilder builder(program);
    for (auto& block : program.Blocks()) {
        builder.SetInsertionPoint(*block);
        for (auto* value : block->Instructions()) {
            const auto imageOpcode = ImageOpcodeInfoOf(value->Opcode());
            if (imageOpcode.access == ImageAccess::None) {
                continue;
            }
            const auto flags = value->Flags<MemoryFlags>();
            if (flags.index >= memoryInfo.size()) {
                throw std::runtime_error("ResourceMaterializer::Apply memory info index is out of range");
            }
            auto& memory = memoryInfo[flags.index];
            if (memory.resource >= images.size()) {
                throw std::runtime_error("ResourceMaterializer::Apply memory resource index is out of range");
            }
            if (specialization.images[memory.resource].fmask) {
                if (value->Opcode() != IrOpcode::ImageRead || memory.dataBits != 32u) {
                    throw std::runtime_error("ResourceMaterializer::Apply found an unsupported FMASK access");
                }
                constexpr std::array<std::uint32_t, 2> fragmentIndices{0x76543210u, 0xfedcba98u};
                std::array<IrValue*, 2> fragments{};
                for (std::uint32_t component = 0; component < fragments.size(); component++) {
                    IrValue& selected = program.CreateValue(IrOpcode::SelectU32, IrType::U32);
                    selected.AddArgument(value->Argument(2));
                    selected.AddArgument(&builder.Constant(fragmentIndices[component]));
                    selected.AddArgument(&builder.Constant(0u));
                    block->InsertInstructionBefore(value, &selected);
                    fragments[component] = &selected;
                }
                IrValue& result = program.CreateValue(IrOpcode::CompositeConstructU32x4, IrType::U32x4);
                result.AddArgument(fragments[0]);
                result.AddArgument(fragments[1]);
                result.AddArgument(&builder.Constant(0u));
                result.AddArgument(&builder.Constant(0u));
                block->InsertInstructionBefore(value, &result);
                value->ReplaceAllUsesWith(&result);
                continue;
            }
            if (imageOpcode.needsSampler && directPoint(memory.resource) && memory.sampler < resources.info.samplers.size() && !tableSampler(memory.sampler)) {
                if (pointSampler[memory.sampler] == NoRemap) {
                    throw std::runtime_error("ResourceMaterializer::Apply missing point sampler for image access");
                }
                memory.sampler = pointSampler[memory.sampler];
            }
        }
    }

    for (auto& memory : memoryInfo) {
        if (memory.kind == ResourceKind::Image && !memory.planningOnly) {
            if (memory.resource >= imageRemap.size() || imageRemap[memory.resource] == NoRemap) {
                throw std::runtime_error("ResourceMaterializer::Apply cannot remap an image memory reference");
            }
            memory.resource = imageRemap[memory.resource];
        }
    }
    for (auto& buffer : buffers) {
        if (buffer.imageAlias != BufferResource::NoImageAlias) {
            if (buffer.imageAlias >= imageRemap.size() || imageRemap[buffer.imageAlias] == NoRemap) {
                throw std::runtime_error("ResourceMaterializer::Apply cannot remap a buffer image alias");
            }
            buffer.imageAlias = imageRemap[buffer.imageAlias];
        }
    }
    for (auto& pair : sampledPairs) {
        if (pair.image >= imageRemap.size() || imageRemap[pair.image] == NoRemap) {
            throw std::runtime_error("ResourceMaterializer::Apply cannot remap a sampled pair image");
        }
        pair.image = imageRemap[pair.image];
    }
    if (images.size() != imageRemap.size()) {
        throw std::runtime_error("ResourceMaterializer::Apply image remap size mismatch");
    }
    for (std::uint32_t index = 0; index < images.size(); index++) {
        if (imageRemap[index] != NoRemap && imageRemap[index] != index) {
            images[imageRemap[index]] = std::move(images[index]);
        }
    }
    images.resize(remapCount);

    resources.info.buffers = std::move(buffers);
    resources.info.images = std::move(images);
    resources.info.samplers = std::move(samplers);
    resources.info.sampledPairs = std::move(sampledPairs);
    resources.info.tableViews = std::move(tableViews);
    resources.memoryInfo = std::move(memoryInfo);
}

namespace {

void ownPlanValues(IrResourcePlan& plan) {
    std::vector<IrValue**> roots;
    for (auto& source : plan.descriptorSources) {
        for (auto& dword : source.dwords) roots.push_back(&dword);
    }
    for (auto& read : plan.srtReads) roots.push_back(&read.value);
    for (auto& block : plan.controlFlow) roots.push_back(&block.condition);
    for (auto& value : plan.uniformFill.values) roots.push_back(&value);

    std::unordered_map<const IrValue*, IrValue*> clones;
    std::vector<const IrValue*> order;
    std::vector<const IrValue*> pending;
    for (const auto* root : roots) {
        if (*root != nullptr) pending.push_back(*root);
    }
    while (!pending.empty()) {
        const auto* value = pending.back();
        pending.pop_back();
        if (!clones.emplace(value, nullptr).second) continue;
        order.push_back(value);
        for (const auto* argument : value->Arguments()) {
            if (argument != nullptr) pending.push_back(argument);
        }
    }
    for (const auto* value : order) {
        auto clone = std::make_unique<IrValue>(value->Opcode(), value->Type(), value->Id());
        clone->SetFlags(value->Flags<std::uint64_t>());
        if (value->HasImmediate()) clone->SetImmediateU64(value->ImmediateU64());
        clone->SetRegister(value->Register());
        clones[value] = clone.get();
        plan.valueStorage.push_back(std::move(clone));
    }
    std::unordered_map<const IrBlock*, IrBlock*> blocks;
    const auto blockFor = [&](const IrBlock* block) {
        auto& clone = blocks[block];
        if (clone == nullptr) {
            plan.blockStorage.push_back(std::make_unique<IrBlock>(block->Id()));
            clone = plan.blockStorage.back().get();
        }
        return clone;
    };
    for (const auto* value : order) {
        auto* clone = clones.at(value);
        for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
            const auto* argument = value->Argument(index);
            auto* mapped = argument == nullptr ? nullptr : clones.at(argument);
            if (value->IsPhi()) {
                clone->AddPhiOperand(blockFor(value->PhiBlock(index)), mapped);
            } else {
                clone->AddArgument(mapped);
            }
        }
    }
    for (auto* root : roots) {
        if (*root != nullptr) *root = clones.at(*root);
    }
}

}

IrResourcePlan ResourceMaterializer::ExtractPlan(const IrProgram& program) const {
    const IrResourcePlan& source = program.Resources();
    if (!source.resourceTrackingComplete || !source.srtPlanComplete) {
        throw std::runtime_error("ResourceMaterializer::ExtractPlan requires a completed resource and SRT plan");
    }
    IrResourcePlan plan;
    plan.stage = source.stage;
    plan.shaderHash = source.shaderHash;
    plan.userDataBase = source.userDataBase;
    plan.userDataCount = source.userDataCount;
    plan.srgbDecodeFormats = source.srgbDecodeFormats;
    plan.memoryInfo = source.memoryInfo;
    plan.descriptorSources = source.descriptorSources;
    plan.controlFlow = source.controlFlow;
    plan.srtReads = source.srtReads;
    plan.cleanFlatSlots = source.cleanFlatSlots;
    plan.requiresSpecializationMemory = source.requiresSpecializationMemory;
    plan.srtPlanComplete = source.srtPlanComplete;
    plan.resourceTrackingComplete = source.resourceTrackingComplete;
    plan.info = source.info;
    plan.uniformFill = source.uniformFill;
    ownPlanValues(plan);
    const auto addSource = [&plan](std::uint32_t index) {
        if (index >= plan.descriptorSources.size()) {
            throw std::runtime_error("ResourceMaterializer::ExtractPlan resource references an unknown descriptor source");
        }
        plan.materializationSources.push_back(index);
    };
    for (const auto& buffer : plan.info.buffers) addSource(buffer.source);
    for (const auto& image : plan.info.images) {
        if (image.source >= plan.descriptorSources.size()) {
            throw std::runtime_error("ResourceMaterializer::ExtractPlan image references an unknown descriptor source");
        }
        if (image.tableView == NoTableView) addSource(image.source);
    }
    for (const auto& sampler : plan.info.samplers) {
        if (sampler.tableView == NoTableView) addSource(sampler.source);
    }
    if (!plan.info.tableViews.empty()) plan.requiresSpecializationMemory = true;
    plan.pureFlatSlots = Detail::ComputePureFlatSlots(plan);
    return plan;
}

void ResourceMaterializer::Materialize(const IrResourcePlan& program, const SrtRuntime& runtime, ResourceSnapshot& snapshot, ResourceSpecialization& specialization) const {
    const IrResourcePlan& plan = program;
    if (!plan.resourceTrackingComplete) {
        throw std::runtime_error("ResourceMaterializer::Materialize requires a completed resource plan");
    }
    if (plan.requiresSpecializationMemory && runtime.readMemory == nullptr) {
        throw std::runtime_error("ResourceMaterializer::Materialize requires runtime memory access for image tables");
    }
    SrtWalker walker;
    ResourceSnapshot nextSnapshot;
    std::vector<std::uint8_t> activeSources;
    try {
        materializeSnapshot(plan, runtime, walker, nextSnapshot, activeSources);
        TableSnapshotter(plan, runtime, walker, activeSources, nextSnapshot.tables).Run();
    } catch (...) {
        reportTables();
        throw;
    }
    ResourceSpecialization nextSpecialization;
    const auto started = MaterializeProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    buildResourceSpecialization(plan, nextSnapshot, nextSpecialization);
    if (MaterializeProfiled()) specializationNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    snapshot = std::move(nextSnapshot);
    specialization = std::move(nextSpecialization);
    reportTables();
}

std::uint64_t ResourceMaterializer::SpecializationNanoseconds() {
    return specializationNanoseconds.load(std::memory_order_relaxed);
}

bool ResourceSpecialization::Buffer::operator==(const Buffer& other) const {
    return packedStride == other.packedStride && descriptorFormat == other.descriptorFormat && descriptorSwizzle == other.descriptorSwizzle && empty == other.empty;
}

bool ResourceSpecialization::Image::operator==(const Image& other) const {
    return numericClass == other.numericClass && dimension == other.dimension && mipCount == other.mipCount && conversionFormat == other.conversionFormat && shaderSwizzle == other.shaderSwizzle && cube == other.cube && fmask == other.fmask && depthBits == other.depthBits && depthUnorm16 == other.depthUnorm16 && packedFormat == other.packedFormat && emulatedCompare == other.emulatedCompare && srgbDecode == other.srgbDecode;
}

bool ResourceSpecialization::operator==(const ResourceSpecialization& other) const {
    return buffers == other.buffers && images == other.images && tableViewClasses == other.tableViewClasses;
}

}

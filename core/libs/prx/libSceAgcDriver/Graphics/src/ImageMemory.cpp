#include "prx/libSceAgcDriver/Graphics/include/ImageMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace AgcDriver::Graphics {

struct ImageMemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t type = 0;
    VkDeviceSize size = 0;
    VkDeviceSize used = 0;
    std::size_t ranges = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;
    std::set<std::pair<VkDeviceSize, VkDeviceSize>> bySize;

    void insertFree(VkDeviceSize offset, VkDeviceSize bytes) {
        free.emplace(offset, bytes);
        bySize.emplace(bytes, offset);
    }

    static VkDeviceSize Aligned(VkDeviceSize offset, VkDeviceSize alignment) {
        return (offset + alignment - 1) / alignment * alignment;
    }

    const std::pair<VkDeviceSize, VkDeviceSize>* Fit(VkDeviceSize bytes, VkDeviceSize alignment) const {
        for (auto candidate = bySize.lower_bound({bytes, 0}); candidate != bySize.end(); ++candidate) {
            if (Aligned(candidate->second, alignment) - candidate->second <= candidate->first - bytes) return &*candidate;
        }
        return nullptr;
    }

    VkDeviceSize Carve(std::pair<VkDeviceSize, VkDeviceSize> range, VkDeviceSize bytes, VkDeviceSize alignment) {
        const auto [rangeBytes, rangeOffset] = range;
        const auto aligned = Aligned(rangeOffset, alignment);
        bySize.erase(range);
        free.erase(rangeOffset);
        const auto end = aligned + bytes;
        if (aligned > rangeOffset) insertFree(rangeOffset, aligned - rangeOffset);
        if (rangeOffset + rangeBytes > end) insertFree(end, rangeOffset + rangeBytes - end);
        used += bytes;
        ++ranges;
        return aligned;
    }

    void Give(VkDeviceSize offset, VkDeviceSize bytes) noexcept {
        used -= bytes;
        --ranges;
        try {
            auto start = offset;
            auto length = bytes;
            auto next = free.lower_bound(offset);
            if (next != free.end() && next->first == offset + bytes) {
                length += next->second;
                bySize.erase({next->second, next->first});
                next = free.erase(next);
            }
            if (next != free.begin()) {
                const auto previous = std::prev(next);
                if (previous->first + previous->second == offset) {
                    start = previous->first;
                    length += previous->second;
                    bySize.erase({previous->second, previous->first});
                    free.erase(previous);
                }
            }
            insertFree(start, length);
        } catch (...) {
        }
    }
};

ImageAllocation::ImageAllocation(ImageAllocation&& other) noexcept : owner(std::move(other.owner)), block(std::exchange(other.block, nullptr)), device(other.device), freeMemory(other.freeMemory), memory(std::exchange(other.memory, VK_NULL_HANDLE)), offset(std::exchange(other.offset, 0)), bytes(std::exchange(other.bytes, 0)), type(other.type) {}

ImageAllocation& ImageAllocation::operator=(ImageAllocation&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    owner = std::move(other.owner);
    block = std::exchange(other.block, nullptr);
    device = other.device;
    freeMemory = other.freeMemory;
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    offset = std::exchange(other.offset, 0);
    bytes = std::exchange(other.bytes, 0);
    type = other.type;
    return *this;
}

ImageAllocation::~ImageAllocation() {
    Reset();
}

void ImageAllocation::Reset() noexcept {
    if (owner) owner->release(*this);
    else if (memory != VK_NULL_HANDLE) freeMemory(device, memory, nullptr);
    owner.reset();
    block = nullptr;
    memory = VK_NULL_HANDLE;
    offset = 0;
    bytes = 0;
}

ImageMemory::ImageMemory(const Context& context) : device(context.device), allocateMemory(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")) {
    static const char* configured = std::getenv("APS5_IMAGE_BLOCK_MIB");
    for (std::uint32_t type = 0; type < context.memory.memoryTypeCount; ++type) {
        const auto heapBytes = context.memory.memoryHeaps[context.memory.memoryTypes[type].heapIndex].size;
        blockBytes[type] = configured != nullptr ? static_cast<VkDeviceSize>(std::strtoull(configured, nullptr, 10)) << 20u : DefaultBlockBytes(heapBytes);
    }
}

ImageMemory::~ImageMemory() {
    for (const auto& list : blocks) {
        for (const auto& block : list) freeMemory(device, block->memory, nullptr);
    }
}

VkDeviceSize ImageMemory::DefaultBlockBytes(VkDeviceSize heapBytes) {
    constexpr VkDeviceSize mebibyte = 1ull << 20u;
    return std::clamp<VkDeviceSize>(heapBytes / 64 / mebibyte * mebibyte, MinBlockBytes, MaxBlockBytes);
}

ImageAllocation ImageMemory::Bind(const Context& context, VkImage image, const char* allocateWhat, const char* bindWhat) {
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated, {}};
    const VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, image};
    context.Function<PFN_vkGetImageMemoryRequirements2>("vkGetImageMemoryRequirements2")(context.device, &info, &requirements);
    const auto& memory = requirements.memoryRequirements;
    const auto type = context.MemoryType(memory.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const bool own = dedicated.prefersDedicatedAllocation == VK_TRUE || dedicated.requiresDedicatedAllocation == VK_TRUE;
    auto allocation = context.imageMemory != nullptr ? context.imageMemory->allocate(type, memory, image, own, allocateWhat) : allocateOwn(context.device, context.Function<PFN_vkAllocateMemory>("vkAllocateMemory"), context.Function<PFN_vkFreeMemory>("vkFreeMemory"), type, memory.size, own ? image : VK_NULL_HANDLE, allocateWhat);
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, allocation.Memory(), allocation.Offset()), bindWhat);
    return allocation;
}

ImageAllocation ImageMemory::allocateOwn(VkDevice device, PFN_vkAllocateMemory allocate, PFN_vkFreeMemory free, std::uint32_t type, VkDeviceSize bytes, VkImage dedicatedImage, const char* what) {
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = dedicatedImage;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext = dedicatedImage != VK_NULL_HANDLE ? &dedicated : nullptr;
    info.allocationSize = bytes;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(allocate(device, &info, nullptr, &memory), what);
    ImageAllocation allocation;
    allocation.device = device;
    allocation.freeMemory = free;
    allocation.memory = memory;
    allocation.bytes = bytes;
    allocation.type = type;
    return allocation;
}

ImageAllocation ImageMemory::allocate(std::uint32_t type, const VkMemoryRequirements& requirements, VkImage image, bool dedicated, const char* what) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto self = shared_from_this();
    const auto bytes = requirements.size;
    const auto own = [&] {
        auto allocation = allocateOwn(device, allocateMemory, freeMemory, type, bytes, dedicated ? image : VK_NULL_HANDLE, what);
        {
            std::lock_guard lock(mutex);
            ++ownAllocations;
            ownBytes += bytes;
        }
        allocation.owner = std::move(self);
        return allocation;
    };
    if (dedicated || bytes > blockBytes[type] / 2) return own();
    const auto alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    std::unique_lock lock(mutex);
    if (profile) reportLocked();
    auto& list = blocks[type];
    ImageMemoryBlock* chosen = nullptr;
    ImageMemoryBlock* spare = nullptr;
    const std::pair<VkDeviceSize, VkDeviceSize>* best = nullptr;
    for (const auto& candidate : list) {
        if (candidate->ranges == 0) {
            spare = candidate.get();
            continue;
        }
        const auto* fit = candidate->Fit(bytes, alignment);
        if (fit != nullptr && (best == nullptr || fit->first < best->first)) {
            best = fit;
            chosen = candidate.get();
        }
    }
    if (chosen == nullptr && spare != nullptr && (best = spare->Fit(bytes, alignment)) != nullptr) chosen = spare;
    VkDeviceSize offset = 0;
    if (chosen != nullptr) {
        offset = chosen->Carve(*best, bytes, alignment);
    } else if (blockRetryBytes[type] != 0) {
        lock.unlock();
        return own();
    } else {
        auto block = std::make_unique<ImageMemoryBlock>();
        block->type = type;
        block->size = blockBytes[type];
        block->insertFree(0, block->size);
        list.reserve(list.size() + 1);
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = block->size;
        info.memoryTypeIndex = type;
        const auto result = allocateMemory(device, &info, nullptr, &block->memory);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) {
            ++blocksRefused;
            blockRetryBytes[type] = blockBytes[type];
            lock.unlock();
            return own();
        }
        Check(result, what);
        ++blocksAllocated;
        list.push_back(std::move(block));
        chosen = list.back().get();
        offset = chosen->Carve(*chosen->bySize.begin(), bytes, alignment);
    }
    ImageAllocation allocation;
    allocation.owner = std::move(self);
    allocation.block = chosen;
    allocation.device = device;
    allocation.freeMemory = freeMemory;
    allocation.memory = chosen->memory;
    allocation.offset = offset;
    allocation.bytes = bytes;
    allocation.type = type;
    return allocation;
}

void ImageMemory::release(ImageAllocation& allocation) noexcept {
    if (allocation.block == nullptr) {
        freeMemory(device, allocation.memory, nullptr);
        std::lock_guard lock(mutex);
        --ownAllocations;
        ownBytes -= allocation.bytes;
        returnedLocked(allocation.type, allocation.bytes);
        return;
    }
    std::unique_ptr<ImageMemoryBlock> gone;
    {
        std::lock_guard lock(mutex);
        auto* block = allocation.block;
        block->Give(allocation.offset, allocation.bytes);
        if (block->ranges == 0) {
            auto& list = blocks[block->type];
            const auto spare = std::any_of(list.begin(), list.end(), [&](const auto& other) { return other.get() != block && other->ranges == 0; });
            if (spare) {
                const auto found = std::find_if(list.begin(), list.end(), [&](const auto& other) { return other.get() == block; });
                gone = std::move(*found);
                list.erase(found);
                ++blocksFreed;
                returnedLocked(block->type, block->size);
            }
        }
    }
    if (gone) freeMemory(device, gone->memory, nullptr);
}

void ImageMemory::returnedLocked(std::uint32_t type, VkDeviceSize bytes) {
    blockRetryBytes[type] -= std::min(blockRetryBytes[type], bytes);
}

ImageMemory::Totals ImageMemory::Statistics() {
    std::lock_guard lock(mutex);
    return totalsLocked();
}

ImageMemory::Totals ImageMemory::totalsLocked() const {
    Totals totals;
    for (const auto& list : blocks) {
        for (const auto& block : list) {
            ++totals.blocks;
            totals.blockBytes += block->size;
            totals.usedBytes += block->used;
            totals.ranges += block->ranges;
        }
    }
    totals.ownAllocations = ownAllocations;
    totals.ownBytes = ownBytes;
    totals.blocksAllocated = blocksAllocated;
    totals.blocksFreed = blocksFreed;
    totals.blocksRefused = blocksRefused;
    return totals;
}

void ImageMemory::reportLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto totals = totalsLocked();
    AgcDriver::ProfilePrint_nid_no_patch("[imagemem] %zu blocks (%.0f MiB), %.0f MiB in %zu images; %zu images in their own allocations (%.0f MiB); blocks allocated %llu, freed %llu, refused %llu\n", totals.blocks, totals.blockBytes / 1048576.0, totals.usedBytes / 1048576.0, totals.ranges, totals.ownAllocations, totals.ownBytes / 1048576.0, static_cast<unsigned long long>(totals.blocksAllocated), static_cast<unsigned long long>(totals.blocksFreed), static_cast<unsigned long long>(totals.blocksRefused));
}

}

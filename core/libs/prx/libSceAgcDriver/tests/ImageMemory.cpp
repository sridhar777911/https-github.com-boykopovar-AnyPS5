#include "prx/libSceAgcDriver/Graphics/include/ImageMemory.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::ImageAllocation;
using AgcDriver::Graphics::ImageMemory;
using AgcDriver::Graphics::Require;

constexpr VkDeviceSize MiB = 1ull << 20u;
constexpr std::uint32_t DeviceType = 1;

struct MockImage {
    VkDeviceSize size = 0;
    VkDeviceSize alignment = 1;
    std::uint32_t typeBits = 0x3;
    bool prefersDedicated = false;
    bool requiresDedicated = false;
    bool failBind = false;
};

struct MockMemory {
    VkDeviceSize size = 0;
    std::uint32_t type = 0;
    VkImage dedicatedImage = VK_NULL_HANDLE;
    std::map<VkDeviceSize, VkDeviceSize> bound;
};

struct MockVulkan {
    std::mutex mutex;
    std::uintptr_t next = 1;
    std::map<VkImage, MockImage> images;
    std::map<VkDeviceMemory, MockMemory> memories;
    std::map<VkImage, std::pair<VkDeviceMemory, VkDeviceSize>> bindings;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t boundFrees = 0;
    std::uint64_t refusals = 0;
    VkDeviceSize refuseFrom = ~VkDeviceSize{0};
    std::vector<VkDeviceSize> allocationSizes;
};

MockVulkan mock;

void resetMock() {
    std::lock_guard lock(mock.mutex);
    mock.images.clear();
    mock.memories.clear();
    mock.bindings.clear();
    mock.allocations = 0;
    mock.frees = 0;
    mock.boundFrees = 0;
    mock.refusals = 0;
    mock.refuseFrom = ~VkDeviceSize{0};
    mock.allocationSizes.clear();
}

VKAPI_ATTR void VKAPI_CALL mockGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* requirements) {
    std::lock_guard lock(mock.mutex);
    const auto& image = mock.images.at(info->image);
    requirements->memoryRequirements = {image.size, image.alignment, image.typeBits};
    for (auto* next = static_cast<VkBaseOutStructure*>(requirements->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) continue;
        auto* dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(next);
        dedicated->prefersDedicatedAllocation = image.prefersDedicated ? VK_TRUE : VK_FALSE;
        dedicated->requiresDedicatedAllocation = image.requiresDedicated ? VK_TRUE : VK_FALSE;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    std::lock_guard lock(mock.mutex);
    if (info->allocationSize >= mock.refuseFrom) {
        ++mock.refusals;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    MockMemory entry;
    entry.size = info->allocationSize;
    entry.type = info->memoryTypeIndex;
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr; next = next->pNext) {
        Require(next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, "unexpected structure chained to an image allocation");
        entry.dedicatedImage = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(next)->image;
    }
    *memory = reinterpret_cast<VkDeviceMemory>(mock.next++);
    mock.memories.emplace(*memory, entry);
    ++mock.allocations;
    mock.allocationSizes.push_back(info->allocationSize);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    std::lock_guard lock(mock.mutex);
    const auto found = mock.memories.find(memory);
    if (found == mock.memories.end() || !found->second.bound.empty()) {
        ++mock.boundFrees;
        return;
    }
    mock.memories.erase(found);
    ++mock.frees;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindImageMemory(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    std::lock_guard lock(mock.mutex);
    const auto& requirements = mock.images.at(image);
    if (requirements.failBind) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto& entry = mock.memories.at(memory);
    Require(offset % requirements.alignment == 0, "image bound at a misaligned offset");
    Require(offset + requirements.size <= entry.size, "image bound past the end of its memory");
    Require(entry.dedicatedImage == VK_NULL_HANDLE || entry.dedicatedImage == image, "image bound to another image's dedicated allocation");
    Require(((requirements.typeBits >> entry.type) & 1u) != 0, "image bound to memory of a type it does not accept");
    const auto after = entry.bound.lower_bound(offset);
    Require(after == entry.bound.end() || after->first >= offset + requirements.size, "image overlaps the next image in its memory");
    if (after != entry.bound.begin()) Require(std::prev(after)->second <= offset, "image overlaps the previous image in its memory");
    entry.bound.emplace(offset, offset + requirements.size);
    mock.bindings[image] = {memory, offset};
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    const std::string_view function(name);
    if (function == "vkGetImageMemoryRequirements2") return reinterpret_cast<PFN_vkVoidFunction>(mockGetImageMemoryRequirements2);
    if (function == "vkAllocateMemory") return reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory);
    if (function == "vkFreeMemory") return reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory);
    if (function == "vkBindImageMemory") return reinterpret_cast<PFN_vkVoidFunction>(mockBindImageMemory);
    return nullptr;
}

Context mockContext(VkDeviceSize deviceHeapBytes) {
    Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    context.memory.memoryTypes[DeviceType] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1};
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {8ull << 30u, 0};
    context.memory.memoryHeaps[1] = {deviceHeapBytes, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    return context;
}

VkImage makeImage(const MockImage& image) {
    std::lock_guard lock(mock.mutex);
    const auto handle = reinterpret_cast<VkImage>(mock.next++);
    mock.images.emplace(handle, image);
    return handle;
}

struct Bound {
    VkImage image = VK_NULL_HANDLE;
    ImageAllocation allocation;
};

Bound bind(const Context& context, const MockImage& requirements) {
    Bound bound;
    bound.image = makeImage(requirements);
    bound.allocation = ImageMemory::Bind(context, bound.image, "vkAllocateMemory texture", "vkBindImageMemory");
    return bound;
}

void unbind(Bound& bound) {
    {
        std::lock_guard lock(mock.mutex);
        const auto binding = mock.bindings.find(bound.image);
        if (binding != mock.bindings.end()) {
            mock.memories.at(binding->second.first).bound.erase(binding->second.second);
            mock.bindings.erase(binding);
        }
    }
    bound.allocation.Reset();
}

VkDeviceSize memorySize(VkDeviceMemory memory) {
    std::lock_guard lock(mock.mutex);
    return mock.memories.at(memory).size;
}

template<typename TAction>
void expectFailure(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected failure: ") + error.what());
        return;
    }
    throw std::runtime_error("expected failure: " + std::string(reason));
}

void expectNoBoundFrees() {
    std::lock_guard lock(mock.mutex);
    Require(mock.boundFrees == 0, "memory was freed while images were bound to it, or freed twice");
}

void blockSizeTests() {
    Require(ImageMemory::DefaultBlockBytes(16ull << 30u) == 256 * MiB, "a 16 GiB heap takes 256 MiB blocks");
    Require(ImageMemory::DefaultBlockBytes(64ull << 30u) == 256 * MiB, "blocks stay at most 256 MiB");
    Require(ImageMemory::DefaultBlockBytes(8ull << 30u) == 128 * MiB, "an 8 GiB heap takes 128 MiB blocks");
    Require(ImageMemory::DefaultBlockBytes(6ull << 30u) == 96 * MiB, "a 6 GiB heap takes 96 MiB blocks");
    Require(ImageMemory::DefaultBlockBytes(16175 * MiB) == 252 * MiB, "a block is a 64th of the heap in whole MiB");
    Require(ImageMemory::DefaultBlockBytes(1ull << 30u) == 16 * MiB, "a 1 GiB heap takes 16 MiB blocks");
    Require(ImageMemory::DefaultBlockBytes(256 * MiB) == 16 * MiB, "blocks stay at least 16 MiB");
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(4ull << 30u));
    Require(memory->BlockBytes(DeviceType) == 64 * MiB, "a memory type's blocks follow its heap");
    Require(memory->BlockBytes(0) == 128 * MiB, "the host type's blocks follow the host heap");
}

void packingTests() {
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    auto context = mockContext(1ull << 30u);
    context.imageMemory = memory.get();
    const VkDeviceSize alignments[] = {256, 1024, 4096, 65536};
    std::vector<Bound> images;
    std::vector<MockImage> sizes;
    VkDeviceSize used = 0;
    for (std::uint32_t i = 0; i < 24; ++i) {
        const MockImage requirements{100 * 1024 + i * 37 * 1024 + 768, alignments[i % 4]};
        sizes.push_back(requirements);
        images.push_back(bind(context, requirements));
        used += requirements.size;
        Require(images.back().allocation.Shared(), "a small image is placed in a shared block");
        Require(images.back().allocation.Bytes() == requirements.size, "an image's range is its required size");
    }
    Require(mock.allocations == 1, "24 small images share one block allocation");
    Require(mock.allocationSizes.at(0) == 16 * MiB, "the block has the heap's block size");
    auto totals = memory->Statistics();
    Require(totals.blocks == 1 && totals.ranges == 24 && totals.usedBytes == used, "the block counts its images and bytes");

    const auto kept = images[3].allocation.Offset();
    for (std::size_t i = 0; i < images.size(); i += 2) unbind(images[i]);
    totals = memory->Statistics();
    Require(totals.ranges == 12 && mock.frees == 0, "released ranges return to the block, which stays allocated");
    for (std::size_t i = 0; i < 24; i += 2) images.push_back(bind(context, sizes[i]));
    Require(mock.allocations == 1, "freed ranges are reused before a new block is allocated");
    Require(images[3].allocation.Offset() == kept, "live images keep their ranges");

    for (auto& image : images) unbind(image);
    totals = memory->Statistics();
    Require(totals.blocks == 1 && totals.ranges == 0 && totals.usedBytes == 0 && mock.frees == 0, "an empty block is kept as the spare");
    auto whole = bind(context, {8 * MiB, 65536});
    Require(whole.allocation.Shared() && whole.allocation.Offset() == 0, "freed neighbours coalesce, so half a block fits at its start again");
    auto rest = bind(context, {8 * MiB, 1});
    Require(rest.allocation.Shared() && rest.allocation.Offset() == 8 * MiB && rest.allocation.Memory() == whole.allocation.Memory(), "the coalesced block holds both halves");
    Require(mock.allocations == 1, "the two halves fit the one block");
    unbind(whole);
    unbind(rest);
    Require(mock.frees == 0, "the spare block stays");
    memory.reset();
    Require(mock.frees == 1 && mock.memories.empty(), "destroying the allocator frees its spare block");
    expectNoBoundFrees();
}

void blockLifetimeTests() {
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    auto context = mockContext(1ull << 30u);
    context.imageMemory = memory.get();
    std::vector<Bound> images;
    for (int i = 0; i < 6; ++i) images.push_back(bind(context, {6 * MiB, 1024}));
    Require(mock.allocations == 3, "six 6 MiB images take three 16 MiB blocks");
    Require(images[0].allocation.Memory() == images[1].allocation.Memory() && images[1].allocation.Memory() != images[2].allocation.Memory(), "two images fill a block before the next one is allocated");
    auto lent = std::move(images[5]);
    for (int i = 0; i < 5; ++i) unbind(images[i]);
    auto totals = memory->Statistics();
    Require(totals.blocks == 2 && totals.blocksFreed == 1 && mock.frees == 1, "a block that empties while another is spare is freed");
    unbind(lent);
    totals = memory->Statistics();
    Require(totals.blocks == 1 && totals.blocksFreed == 2 && mock.frees == 2 && totals.blocksAllocated == 3, "one empty block is kept");
    auto again = bind(context, {6 * MiB, 1024});
    Require(mock.allocations == 3, "the spare block serves the next image");
    unbind(again);
    auto held = bind(context, {1 * MiB, 1024});
    memory.reset();
    Require(mock.frees == 2, "the allocator lives while an image holds a range");
    unbind(held);
    Require(mock.frees == 3 && mock.memories.empty(), "the last released range frees the allocator and its block");
    expectNoBoundFrees();
}

void dedicatedTests() {
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    auto context = mockContext(1ull << 30u);
    context.imageMemory = memory.get();
    auto prefers = bind(context, {1 * MiB, 1024, 0x3, true, false});
    Require(!prefers.allocation.Shared() && prefers.allocation.Offset() == 0, "an image whose driver prefers a dedicated allocation gets its own");
    Require(memorySize(prefers.allocation.Memory()) == 1 * MiB, "an own allocation has the image's size");
    {
        std::lock_guard lock(mock.mutex);
        Require(mock.memories.at(prefers.allocation.Memory()).dedicatedImage == prefers.image, "the preferred dedicated allocation names its image");
    }
    auto required = bind(context, {2 * MiB, 1024, 0x3, false, true});
    Require(!required.allocation.Shared(), "an image that requires a dedicated allocation gets its own");
    auto large = bind(context, {8 * MiB + 1, 1024});
    Require(!large.allocation.Shared() && memorySize(large.allocation.Memory()) == 8 * MiB + 1, "an image over half a block gets an allocation of its size");
    {
        std::lock_guard lock(mock.mutex);
        Require(mock.memories.at(large.allocation.Memory()).dedicatedImage == VK_NULL_HANDLE, "a large image's own allocation is not marked dedicated");
        Require(mock.memories.at(large.allocation.Memory()).type == DeviceType, "images use the device-local type");
    }
    auto totals = memory->Statistics();
    Require(totals.blocks == 0 && totals.ownAllocations == 3 && totals.ownBytes == 11 * MiB + 1, "own allocations are counted apart from blocks");
    unbind(prefers);
    unbind(required);
    unbind(large);
    totals = memory->Statistics();
    Require(totals.ownAllocations == 0 && totals.ownBytes == 0 && mock.frees == 3 && mock.memories.empty(), "own allocations are freed with their images");

    auto own = context;
    own.imageMemory = nullptr;
    auto plain = bind(own, {3 * MiB, 4096});
    Require(!plain.allocation.Shared() && memorySize(plain.allocation.Memory()) == 3 * MiB, "without an allocator every image gets its own allocation");
    unbind(plain);
    Require(mock.memories.empty(), "an image's own allocation is freed with it");
    expectNoBoundFrees();
}

void outOfMemoryTests() {
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    auto context = mockContext(1ull << 30u);
    context.imageMemory = memory.get();
    mock.refuseFrom = 16 * MiB;
    auto fallback = bind(context, {1 * MiB, 1024});
    Require(!fallback.allocation.Shared() && memorySize(fallback.allocation.Memory()) == 1 * MiB, "a refused block leaves the image an allocation of its own size");
    auto totals = memory->Statistics();
    Require(totals.blocksRefused == 1 && totals.blocks == 0 && totals.ownAllocations == 1, "the refused block is counted");
    auto large = bind(context, {14 * MiB, 1024});
    auto next = bind(context, {1 * MiB, 1024});
    Require(!next.allocation.Shared() && mock.refusals == 1, "after a refused block the next image takes its own allocation without trying a block");
    mock.refuseFrom = 1;
    expectFailure([&] { bind(context, {1 * MiB, 1024}); }, "vkAllocateMemory texture: Vulkan result -2");
    totals = memory->Statistics();
    Require(totals.blocksRefused == 1 && totals.ownAllocations == 3 && mock.refusals == 2, "a refused image allocation throws and keeps nothing");
    mock.refuseFrom = ~VkDeviceSize{0};
    unbind(next);
    unbind(large);
    auto waiting = bind(context, {1 * MiB, 1024});
    Require(!waiting.allocation.Shared(), "no block is tried before a block's worth of memory was freed");
    unbind(waiting);
    auto shared = bind(context, {1 * MiB, 1024});
    Require(shared.allocation.Shared() && memory->Statistics().blocksAllocated == 1, "a block is tried again once a block's worth of memory was freed");
    expectFailure([&] { bind(context, {1 * MiB, 1024, 0x3, false, false, true}); }, "vkBindImageMemory: Vulkan result -2");
    totals = memory->Statistics();
    Require(totals.ranges == 1 && totals.usedBytes == 1 * MiB, "a failed bind returns its range");
    unbind(fallback);
    unbind(shared);
    memory.reset();
    Require(mock.memories.empty(), "nothing leaks after refusals");
    expectNoBoundFrees();

    resetMock();
    memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    context.imageMemory = memory.get();
    std::vector<Bound> images;
    for (int i = 0; i < 6; ++i) images.push_back(bind(context, {6 * MiB, 1024}));
    mock.refuseFrom = 16 * MiB;
    auto refused = bind(context, {6 * MiB, 1024});
    Require(!refused.allocation.Shared() && memory->Statistics().blocksRefused == 1, "a fourth block is refused");
    mock.refuseFrom = ~VkDeviceSize{0};
    for (int i = 0; i < 4; ++i) unbind(images[i]);
    Require(memory->Statistics().blocksFreed == 1, "the second empty block is freed");
    for (int i = 0; i < 3; ++i) images.push_back(bind(context, {6 * MiB, 1024}));
    Require(images.back().allocation.Shared() && memory->Statistics().blocksAllocated == 4, "a freed block counts as a block's worth of memory");
    for (auto& image : images) unbind(image);
    unbind(refused);
    memory.reset();
    Require(mock.memories.empty(), "nothing leaks after a freed block");
    expectNoBoundFrees();
}

void concurrencyTests() {
    resetMock();
    auto memory = std::make_shared<ImageMemory>(mockContext(1ull << 30u));
    auto context = mockContext(1ull << 30u);
    context.imageMemory = memory.get();
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (std::uint32_t seed = 0; seed < 4; ++seed) {
        threads.emplace_back([&, seed] {
            try {
                std::mt19937 random(seed);
                std::vector<Bound> live;
                for (int step = 0; step < 1500; ++step) {
                    if (!live.empty() && (live.size() > 40 || random() % 3 == 0)) {
                        const auto index = random() % live.size();
                        unbind(live[index]);
                        live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
                    } else {
                        live.push_back(bind(context, {(random() % 512 + 1) * 1024, VkDeviceSize{256} << (random() % 9)}));
                    }
                }
                for (auto& image : live) unbind(image);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                failed = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Require(!failed, "concurrent binds and releases keep ranges disjoint");
    const auto totals = memory->Statistics();
    Require(totals.ranges == 0 && totals.usedBytes == 0 && totals.blocks == 1, "every range came back and one spare block is left");
    memory.reset();
    Require(mock.memories.empty(), "concurrent use leaks no memory");
    expectNoBoundFrees();
}

void clearEnvironment(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

}

int main() {
    clearEnvironment("APS5_IMAGE_BLOCK_MIB");
    try {
        blockSizeTests();
        packingTests();
        blockLifetimeTests();
        dedicatedTests();
        outOfMemoryTests();
        concurrencyTests();
        std::cout << "Image memory tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

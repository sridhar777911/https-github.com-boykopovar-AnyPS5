#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {

class ImageMemory;
struct ImageMemoryBlock;

class ImageAllocation {
public:
    ImageAllocation() = default;
    ImageAllocation(const ImageAllocation&) = delete;
    ImageAllocation& operator=(const ImageAllocation&) = delete;
    ImageAllocation(ImageAllocation&& other) noexcept;
    ImageAllocation& operator=(ImageAllocation&& other) noexcept;
    ~ImageAllocation();
    void Reset() noexcept;
    VkDeviceMemory Memory() const { return memory; }
    VkDeviceSize Offset() const { return offset; }
    VkDeviceSize Bytes() const { return bytes; }
    bool Shared() const { return block != nullptr; }

private:
    friend class ImageMemory;
    std::shared_ptr<ImageMemory> owner;
    ImageMemoryBlock* block = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkFreeMemory freeMemory = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize bytes = 0;
    std::uint32_t type = 0;
};

class ImageMemory : public std::enable_shared_from_this<ImageMemory> {
public:
    explicit ImageMemory(const Context& context);
    ~ImageMemory();
    ImageMemory(const ImageMemory&) = delete;
    ImageMemory& operator=(const ImageMemory&) = delete;

    static ImageAllocation Bind(const Context& context, VkImage image, const char* allocateWhat, const char* bindWhat);
    static VkDeviceSize DefaultBlockBytes(VkDeviceSize heapBytes);
    VkDeviceSize BlockBytes(std::uint32_t type) const { return blockBytes[type]; }

    struct Totals {
        std::size_t blocks = 0;
        VkDeviceSize blockBytes = 0;
        VkDeviceSize usedBytes = 0;
        std::size_t ranges = 0;
        std::size_t ownAllocations = 0;
        VkDeviceSize ownBytes = 0;
        std::uint64_t blocksAllocated = 0;
        std::uint64_t blocksFreed = 0;
        std::uint64_t blocksRefused = 0;
    };
    Totals Statistics();

    static constexpr VkDeviceSize MinBlockBytes = 16ull << 20u;
    static constexpr VkDeviceSize MaxBlockBytes = 256ull << 20u;

private:
    friend class ImageAllocation;
    static ImageAllocation allocateOwn(VkDevice device, PFN_vkAllocateMemory allocate, PFN_vkFreeMemory free, std::uint32_t type, VkDeviceSize bytes, VkImage dedicatedImage, const char* what);
    ImageAllocation allocate(std::uint32_t type, const VkMemoryRequirements& requirements, VkImage image, bool dedicated, const char* what);
    void release(ImageAllocation& allocation) noexcept;
    Totals totalsLocked() const;
    void returnedLocked(std::uint32_t type, VkDeviceSize bytes);
    void reportLocked();

    VkDevice device;
    PFN_vkAllocateMemory allocateMemory;
    PFN_vkFreeMemory freeMemory;
    std::array<VkDeviceSize, VK_MAX_MEMORY_TYPES> blockBytes{};
    std::mutex mutex;
    std::array<std::vector<std::unique_ptr<ImageMemoryBlock>>, VK_MAX_MEMORY_TYPES> blocks;
    std::array<VkDeviceSize, VK_MAX_MEMORY_TYPES> blockRetryBytes{};
    std::size_t ownAllocations = 0;
    VkDeviceSize ownBytes = 0;
    std::uint64_t blocksAllocated = 0;
    std::uint64_t blocksFreed = 0;
    std::uint64_t blocksRefused = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

}

#endif

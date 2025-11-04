#pragma once

#include <cstdint>
#include <unordered_map>
#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "VulkanTypes.hpp"

class VulkanDevice;

// Tracking structure for buffer lifecycle debugging
struct BufferAllocation {
    uint64_t id;
    VkBuffer buffer;
    VmaAllocation allocation;
    size_t size;
    const char* purpose;
};

class VulkanBuffer {
  public:
    VulkanBuffer(VulkanDevice& device);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) = delete;
    VulkanBuffer& operator=(VulkanBuffer&& other) = delete;

    AllocatedBuffer createBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
    void destroyBuffer(const AllocatedBuffer& buffer);

    void uploadToBuffer(const AllocatedBuffer& dst, const void* data, size_t size);
    AllocatedBuffer createStagingBuffer(size_t size);

    // Get tracking stats (for debugging)
    [[nodiscard]] const std::unordered_map<VkBuffer, BufferAllocation>&
    getActiveAllocations() const {
        return _activeAllocations;
    }
    [[nodiscard]] size_t getTotalVmaAllocations() const { return _totalVmaAllocations; }
    [[nodiscard]] size_t getTotalVmaDeallocations() const { return _totalVmaDeallocations; }

  private:
    VulkanDevice& _device;

    // Buffer lifecycle tracking (for leak detection)
    std::unordered_map<VkBuffer, BufferAllocation> _activeAllocations;
    uint64_t _nextBufferId = 1;
    size_t _totalVmaAllocations = 0;
    size_t _totalVmaDeallocations = 0;
};

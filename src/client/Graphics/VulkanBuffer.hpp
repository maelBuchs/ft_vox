#pragma once

#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "VulkanTypes.hpp"

struct VulkanDevice;

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

  private:
    VulkanDevice& _device;
};

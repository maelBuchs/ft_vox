#include "VulkanBuffer.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

#include "VulkanDevice.hpp"

VulkanBuffer::VulkanBuffer(VulkanDevice& device) : _device(device) {}

VulkanBuffer::~VulkanBuffer() = default;

AllocatedBuffer VulkanBuffer::createBuffer(size_t size, VkBufferUsageFlags usage,
                                           VmaMemoryUsage memoryUsage) {
    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .pNext = nullptr,
                                  .flags = 0,
                                  .size = size,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .queueFamilyIndexCount = 0,
                                  .pQueueFamilyIndices = nullptr};

    VmaAllocationCreateInfo allocInfo{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .usage = memoryUsage,
                                      .requiredFlags = 0,
                                      .preferredFlags = 0,
                                      .memoryTypeBits = 0,
                                      .pool = VK_NULL_HANDLE,
                                      .pUserData = nullptr,
                                      .priority = 0.0F};

    AllocatedBuffer newBuffer{};

    VkResult result = vmaCreateBuffer(_device.getAllocator(), &bufferInfo, &allocInfo,
                                      &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info);

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    // Track this allocation for leak detection
    BufferAllocation tracked{.id = _nextBufferId++,
                             .buffer = newBuffer.buffer,
                             .allocation = newBuffer.allocation,
                             .size = size,
                             .purpose = "buffer"};

    _activeAllocations[newBuffer.buffer] = tracked;
    _totalVmaAllocations++;

    return newBuffer;
}

void VulkanBuffer::destroyBuffer(const AllocatedBuffer& buffer) {
    if (buffer.buffer == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE) {
        return;
    }

    auto it = _activeAllocations.find(buffer.buffer);
    if (it == _activeAllocations.end()) {
        return;
    }

    const BufferAllocation& tracked = it->second;
    if (tracked.allocation != buffer.allocation) {
        return;
    }

    vmaDestroyBuffer(_device.getAllocator(), buffer.buffer, buffer.allocation);
    _activeAllocations.erase(it);
    _totalVmaDeallocations++;
}

void VulkanBuffer::uploadToBuffer(const AllocatedBuffer& dst, const void* data, size_t size) {
    if (dst.info.pMappedData == nullptr) {
        throw std::runtime_error("Buffer is not mapped");
    }

    std::memcpy(dst.info.pMappedData, data, size);
}

AllocatedBuffer VulkanBuffer::createStagingBuffer(size_t size) {
    return createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
}

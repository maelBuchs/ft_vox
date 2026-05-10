#include "VulkanBuffer.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

#include "VulkanDevice.hpp"

VulkanBuffer::VulkanBuffer(VulkanDevice& device) : _device(device) {}

VulkanBuffer::~VulkanBuffer() {
    for (const auto& [buffer, tracked] : _activeAllocations) {
        std::cerr << "[VulkanBuffer] WARNING: Leaked buffer id=" << tracked.id
                  << " size=" << tracked.size << " purpose=" << tracked.purpose << std::endl;
        vmaDestroyBuffer(_device.getAllocator(), buffer, tracked.allocation);
    }
    _activeAllocations.clear();

    for (auto& [memType, pool] : _customPools) {
        vmaDestroyPool(_device.getAllocator(), pool);
    }
    _customPools.clear();
}

VmaPool VulkanBuffer::getPoolForMemoryType(uint32_t memoryTypeIndex, VkDeviceSize blockSize) {
    auto it = _customPools.find(memoryTypeIndex);
    if (it != _customPools.end()) {
        return it->second;
    }

    VmaPoolCreateInfo poolInfo = {};
    poolInfo.memoryTypeIndex = memoryTypeIndex;
    poolInfo.blockSize = blockSize;
    poolInfo.minBlockCount = 0;
    poolInfo.maxBlockCount = 0;

    VmaPool newPool = VK_NULL_HANDLE;
    VkResult res = vmaCreatePool(_device.getAllocator(), &poolInfo, &newPool);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("Failed to create custom VMA pool");
    }

    _customPools[memoryTypeIndex] = newPool;
    return newPool;
}

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

    VmaAllocationCreateFlags allocFlags = 0;
    VmaMemoryUsage actualUsage = VMA_MEMORY_USAGE_AUTO;

    if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY) {
        allocFlags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        actualUsage = VMA_MEMORY_USAGE_AUTO;
    } else if (memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) {
        allocFlags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        actualUsage = VMA_MEMORY_USAGE_AUTO;
    } else if (memoryUsage == VMA_MEMORY_USAGE_GPU_TO_CPU) {
        allocFlags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        actualUsage = VMA_MEMORY_USAGE_AUTO;
    } else if (memoryUsage == VMA_MEMORY_USAGE_GPU_ONLY) {
        actualUsage = VMA_MEMORY_USAGE_AUTO; // Will be placed in device local memory
    } else if (memoryUsage == VMA_MEMORY_USAGE_AUTO) {
        // If caller passed AUTO directly, assume they don't want it mapped unless they specify otherwise.
        // But since we can't tell, we assume unmapped.
        actualUsage = VMA_MEMORY_USAGE_AUTO;
    }

    VmaAllocationCreateInfo allocInfo{.flags = allocFlags,
                                      .usage = actualUsage,
                                      .requiredFlags = 0,
                                      .preferredFlags = 0,
                                      .memoryTypeBits = 0,
                                      .pool = VK_NULL_HANDLE,
                                      .pUserData = nullptr,
                                      .priority = 0.0F};

    if (size < 1048576) {
        uint32_t memTypeIndex = UINT32_MAX;
        vmaFindMemoryTypeIndexForBufferInfo(_device.getAllocator(), &bufferInfo, &allocInfo, &memTypeIndex);

        if (memTypeIndex == UINT32_MAX) {
            // Fallback: manually find a suitable memory type index
            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(_device.getPhysicalDevice(), &memProps);
            VkMemoryPropertyFlags targetFlags = (actualUsage == VMA_MEMORY_USAGE_AUTO && (allocFlags & VMA_ALLOCATION_CREATE_MAPPED_BIT) == 0)
                                                ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                                : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memProps.memoryTypes[i].propertyFlags & targetFlags) == targetFlags) {
                    memTypeIndex = i;
                    break;
                }
            }
        }

        if (memTypeIndex != UINT32_MAX) {
            // Use a custom pool with 256MB block size for sub-allocations
            allocInfo.pool = getPoolForMemoryType(memTypeIndex, 256ull * 1024 * 1024);

            // To be absolutely certain VMA sub-allocates, we try NEVER_ALLOCATE first.
            // If the pool is empty, we must allow it to allocate the FIRST block, so we don't
            // actually use NEVER_ALLOCATE_BIT unless we know the pool has blocks.
            // Instead, we can just ensure VMA's pool logic doesn't override us.
            // Actually, we can just remove any DEDICATED_MEMORY bits if present.
        }
    }

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
        std::cerr << "[VulkanBuffer] destroyBuffer: buffer not found in tracking!"
                  << std::endl;
        return;
    }

    const BufferAllocation& tracked = it->second;
    if (tracked.allocation != buffer.allocation) {
        std::cerr << "[VulkanBuffer] destroyBuffer: allocation mismatch! tracked="
                  << tracked.allocation << " vs passed=" << buffer.allocation << std::endl;
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

    vmaFlushAllocation(_device.getAllocator(), dst.allocation, 0, size);
}

AllocatedBuffer VulkanBuffer::createStagingBuffer(size_t size) {
    return createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
}

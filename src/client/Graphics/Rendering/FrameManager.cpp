#include "FrameManager.hpp"

#include <stdexcept>

#include "../Core/VulkanDevice.hpp"

FrameManager::FrameManager(VulkanDevice& device) : _device(device), _frameNumber(0) {
    createFrameCommandPools();
    createFrameSyncStructures();
    initFrameDescriptors();
}

FrameManager::~FrameManager() {
    for (auto& frame : _frameData) {
        frame._deletionQueue.flush();
    }
    _frameDeletionQueue.flush();
}

FrameManager::FrameData& FrameManager::getCurrentFrame() {
    return _frameData.at(_frameNumber % FRAME_OVERLAP);
}

void FrameManager::createFrameCommandPools() {
    VkCommandPoolCreateInfo commandPoolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags =
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = _device.getGraphicsQueueFamily()};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult res = vkCreateCommandPool(_device.getDevice(), &commandPoolInfo, nullptr,
                                           &_frameData.at(i)._commandPool);
        if (res != VK_SUCCESS) {
            throw std::runtime_error("Failed to create command pool");
        }

        _frameDeletionQueue.push([this, i]() {
            vkDestroyCommandPool(_device.getDevice(), _frameData.at(i)._commandPool, nullptr);
        });

        VkCommandBufferAllocateInfo cmdAllocInfo{.sType =
                                                     VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .pNext = nullptr,
                                                 .commandPool = _frameData.at(i)._commandPool,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};

        VkResult allocRes = vkAllocateCommandBuffers(_device.getDevice(), &cmdAllocInfo,
                                                     &_frameData.at(i)._mainCommandBuffer);
        if (allocRes != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate command buffers");
        }
    }
}

void FrameManager::createFrameSyncStructures() {
    VkFenceCreateInfo fenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult fenceRes = vkCreateFence(_device.getDevice(), &fenceCreateInfo, nullptr,
                                          &_frameData.at(i)._renderFence);
        if (fenceRes != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render fence");
        }

        _frameDeletionQueue.push([this, i]() {
            vkDestroyFence(_device.getDevice(), _frameData.at(i)._renderFence, nullptr);
        });
    }
}

void FrameManager::initFrameDescriptors() {
    for (size_t i = 0; i < FRAME_OVERLAP; i++) {
        std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes = {
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .ratio = 4.0F},
        };

        _frameData[i]._frameDescriptors.init(_device.getDevice(), 1000, frameSizes);

        _frameDeletionQueue.push(
            [this, i]() { _frameData[i]._frameDescriptors.destroyPools(_device.getDevice()); });
    }
}

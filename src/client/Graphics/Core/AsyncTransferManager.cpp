#include "AsyncTransferManager.hpp"

#include <iostream>
#include <stdexcept>

#include "VulkanDevice.hpp"

AsyncTransferManager::AsyncTransferManager(VulkanDevice& device) : _device(device) {
    // Check if device supports async transfer
    if (!_device.hasAsyncTransferQueue()) {
        _available = false;
        return;
    }

    VkDevice vkDevice = _device.getDevice();

    // Create command pool for transfer queue
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = _device.getTransferQueueFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &_transferCmdPool) != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to create transfer command pool");
    }

    // Allocate command buffers (one per frame)
    VkCommandBufferAllocateInfo cmdBufInfo{};
    cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdBufInfo.commandPool = _transferCmdPool;
    cmdBufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufInfo.commandBufferCount = FRAME_COUNT;

    if (vkAllocateCommandBuffers(vkDevice, &cmdBufInfo, _transferCmdBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to allocate transfer command buffers");
    }

    // Create semaphores for transfer → graphics synchronization
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &_transferSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("[AsyncTransferManager] Failed to create transfer semaphore");
        }
    }

    // Create fences to track transfer completion
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled (no transfer in flight)

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &_transferFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("[AsyncTransferManager] Failed to create transfer fence");
        }
    }

    _available = true;
}

AsyncTransferManager::~AsyncTransferManager() {
    if (!_available) {
        return; // Nothing to clean up
    }

    VkDevice vkDevice = _device.getDevice();

    // Wait for all transfers to complete
    VkResult idleResult = vkDeviceWaitIdle(vkDevice);
    if (idleResult == VK_ERROR_DEVICE_LOST) {
        _device.logDeviceFaultInfo("[AsyncTransferManager] vkDeviceWaitIdle during cleanup");
    }

    // Destroy fences
    for (auto fence : _transferFences) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(vkDevice, fence, nullptr);
        }
    }

    // Destroy semaphores
    for (auto semaphore : _transferSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vkDevice, semaphore, nullptr);
        }
    }

    // Destroy command pool (automatically frees command buffers)
    if (_transferCmdPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vkDevice, _transferCmdPool, nullptr);
    }
}

VkSemaphore AsyncTransferManager::submitTransfer(
    const std::function<void(VkCommandBuffer)>& recordFunc, uint32_t frameIndex) {

    if (!_available) {
        return VK_NULL_HANDLE; // Async transfer not available
    }

    const uint32_t frame = frameIndex % FRAME_COUNT;
    VkDevice vkDevice = _device.getDevice();
    VkCommandBuffer cmd = _transferCmdBuffers[frame];
    VkFence fence = _transferFences[frame];
    VkSemaphore semaphore = _transferSemaphores[frame];

    // Wait for previous transfer on this frame index to complete
    VkResult waitResult = vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    if (waitResult == VK_ERROR_DEVICE_LOST) {
        _device.logDeviceFaultInfo("[AsyncTransferManager] vkWaitForFences");
    }
    if (waitResult != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to wait for transfer fence");
    }

    VkResult resetResult = vkResetFences(vkDevice, 1, &fence);
    if (resetResult != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to reset transfer fence");
    }

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to begin transfer command buffer");
    }

    // Record transfer commands
    recordFunc(cmd);

    // End command buffer
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to end transfer command buffer");
    }

    // Submit to transfer queue
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &semaphore; // Signal when transfer completes

    VkQueue transferQueue = _device.getTransferQueue();
    VkResult submitResult = vkQueueSubmit(transferQueue, 1, &submitInfo, fence);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        _device.logDeviceFaultInfo("[AsyncTransferManager] vkQueueSubmit");
    }
    if (submitResult != VK_SUCCESS) {
        throw std::runtime_error("[AsyncTransferManager] Failed to submit to transfer queue");
    }

    // Return semaphore for graphics queue to wait on
    return semaphore;
}

void AsyncTransferManager::advanceFrame() {
    _currentFrame = (_currentFrame + 1) % FRAME_COUNT;
}

#pragma once

#include <array>
#include <functional>
#include <vulkan/vulkan.h>

class VulkanDevice;

class AsyncTransferManager {
  public:
    AsyncTransferManager(VulkanDevice& device);
    ~AsyncTransferManager();

    AsyncTransferManager(const AsyncTransferManager&) = delete;
    AsyncTransferManager& operator=(const AsyncTransferManager&) = delete;

    // Check if async transfer is available
    [[nodiscard]] bool isAvailable() const { return _available; }

    // Submit a transfer operation asynchronously
    // recordFunc: Lambda that records vkCmdCopyBuffer commands
    // Returns: Semaphore that graphics queue should wait on (or VK_NULL_HANDLE if unavailable)
    VkSemaphore submitTransfer(const std::function<void(VkCommandBuffer)>& recordFunc,
                               uint32_t frameIndex);

    // Call this each frame to advance the frame index
    void advanceFrame();

  private:
    VulkanDevice& _device;
    bool _available = false;

    // Transfer command pool and buffers (per-frame)
    static constexpr uint32_t FRAME_COUNT = 2;
    VkCommandPool _transferCmdPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, FRAME_COUNT> _transferCmdBuffers = {};

    // Semaphores for transfer → graphics synchronization (per-frame)
    std::array<VkSemaphore, FRAME_COUNT> _transferSemaphores = {};

    // Fences to track transfer completion (per-frame)
    std::array<VkFence, FRAME_COUNT> _transferFences = {};

    uint32_t _currentFrame = 0;
};

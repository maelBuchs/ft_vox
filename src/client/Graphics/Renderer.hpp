#pragma once

#include <array>
#include <memory>

#include <vulkan/vulkan.h>

class VulkanDevice;
class VulkanSwapchain;
class Window;

class Renderer {
  public:
    Renderer(Window& window, VulkanDevice& device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    struct FrameData {
        VkCommandPool _commandPool;
        VkCommandBuffer _mainCommandBuffer;
        VkFence _renderFence;
        VkSemaphore _swapchainSemaphore;
        VkSemaphore _renderSemaphore;
    };

    static constexpr unsigned int FRAME_OVERLAP = 2;
    static constexpr uint64_t VULKAN_TIMEOUT_NS = 1000000000; // 1 second

    FrameData& getCurrentFrame() { return _frameData.at(_frameNumber % FRAME_OVERLAP); }
    void draw();

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void createFrameCommandPools();
    void createFrameSyncStructures();
    VkImageMemoryBarrier2 createImageBarrier(VkImage image, VkImageLayout oldLayout,
                                             VkImageLayout newLayout) const;
    static VkImageAspectFlags getImageAspectMask(VkImageLayout layout);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout) const;

    Window& _window;
    VulkanDevice& _device;
    std::unique_ptr<VulkanSwapchain> _swapchain;
    uint64_t _frameNumber;
    std::array<FrameData, FRAME_OVERLAP> _frameData;
};

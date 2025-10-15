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

    FrameData& getCurrentFrame() { return _frameData.at(_frameNumber % FRAME_OVERLAP); }
    void draw();

  private:
    Window& _window;
    VulkanDevice& _device;
    std::unique_ptr<VulkanSwapchain> _swapchain;
    uint64_t _frameNumber;
    std::array<FrameData, FRAME_OVERLAP> _frameData;
};

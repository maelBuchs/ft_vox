#pragma once

#include <array>
#include <memory>
#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include "DeletionQueue.hpp"
#include "DescriptorAllocator.hpp"

class VulkanDevice;
class VulkanSwapchain;
class Window;
class VulkanBuffer;
class MeshManager;

class Renderer {
  public:
    Renderer(Window& window, VulkanDevice& device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    struct FrameData {
        VkCommandPool _commandPool{};
        VkCommandBuffer _mainCommandBuffer{};
        VkFence _renderFence{};
        VkSemaphore _swapchainSemaphore{};
        VkSemaphore _renderSemaphore{};
        DeletionQueue _deletionQueue;
        DescriptorAllocatorGrowable _frameDescriptors;
    };

    struct AllocatedImage {
        VkImage image;
        VkImageView imageView;
        VmaAllocation allocation;
        VkExtent3D extent;
        VkFormat format;
    };

    static constexpr unsigned int FRAME_OVERLAP = 2;
    static constexpr uint64_t VULKAN_TIMEOUT_NS = 1000000000; // 1 second

    FrameData& getCurrentFrame() { return _frameData.at(_frameNumber % FRAME_OVERLAP); }
    [[nodiscard]] bool isResizeRequested() const { return _resizeRequested; }
    void draw();
    void resizeSwapchain();

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void createFrameCommandPools();
    void createFrameSyncStructures();
    VkImageMemoryBarrier2 createImageBarrier(VkImage image, VkImageLayout oldLayout,
                                             VkImageLayout newLayout) const;
    static VkImageAspectFlags getImageAspectMask(VkImageLayout layout);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout) const;
    void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination,
                             VkExtent2D srcSize, VkExtent2D dstSize);
    void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
    void initImGui();
    void createDrawImages(VkExtent2D extent);
    void destroyDrawImages();

    Window& _window;
    VulkanDevice& _device;
    std::unique_ptr<VulkanSwapchain> _swapchain;
    DescriptorAllocatorGrowable _globalDescriptorAllocator;
    uint64_t _frameNumber;
    std::array<FrameData, FRAME_OVERLAP> _frameData;
    AllocatedImage _drawImage;
    AllocatedImage _depthImage;
    VkExtent2D _drawExtent;
    DeletionQueue _mainDeletionQueue;
    VkFence _immFence;
    VkCommandPool _immCommandPool;
    VkCommandBuffer _immCommandBuffer;
    std::unique_ptr<VulkanBuffer> _bufferManager;
    std::unique_ptr<MeshManager> _meshManager;
    bool _resizeRequested = false;
};

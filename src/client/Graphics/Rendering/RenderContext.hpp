#pragma once

#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include "../Core/DeletionQueue.hpp"

class VulkanDevice;

class RenderContext {
  public:
    struct AllocatedImage {
        VkImage image;
        VkImageView imageView;
        VmaAllocation allocation;
        VkExtent3D extent;
        VkFormat format;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    explicit RenderContext(VulkanDevice& device);
    ~RenderContext();

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;
    RenderContext(RenderContext&&) = delete;
    RenderContext& operator=(RenderContext&&) = delete;

    void createDrawImages(VkExtent2D extent);
    void destroyDrawImages();
    void createImmediateSubmitStructures();
    void setCurrentFrameIndex(uint32_t frameIndex) { _currentFrameIndex = frameIndex; }

    [[nodiscard]] AllocatedImage& getDrawImage() { return _drawImages[_currentFrameIndex]; }
    [[nodiscard]] AllocatedImage& getDepthImage() { return _depthImages[_currentFrameIndex]; }
    [[nodiscard]] const AllocatedImage& getDrawImage() const { return _drawImages[_currentFrameIndex]; }
    [[nodiscard]] const AllocatedImage& getDepthImage() const { return _depthImages[_currentFrameIndex]; }
    [[nodiscard]] VkExtent2D getDrawExtent() const { return _drawExtent; }
    [[nodiscard]] VkFence getImmediateFence() const { return _immFence; }
    [[nodiscard]] VkCommandPool getImmediateCommandPool() const { return _immCommandPool; }
    [[nodiscard]] VkCommandBuffer getImmediateCommandBuffer() const { return _immCommandBuffer; }

  private:
    VulkanDevice& _device;
    AllocatedImage _drawImages[2]; // Using 2 for FRAME_OVERLAP
    AllocatedImage _depthImages[2];
    VkExtent2D _drawExtent;
    uint32_t _currentFrameIndex = 0;

    // Immediate submit structures
    VkFence _immFence;
    VkCommandPool _immCommandPool;
    VkCommandBuffer _immCommandBuffer;

    DeletionQueue _deletionQueue;
};

#pragma once

#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include "DeletionQueue.hpp"

class VulkanDevice;

class RenderContext {
  public:
    struct AllocatedImage {
        VkImage image;
        VkImageView imageView;
        VmaAllocation allocation;
        VkExtent3D extent;
        VkFormat format;
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

    [[nodiscard]] const AllocatedImage& getDrawImage() const { return _drawImage; }
    [[nodiscard]] const AllocatedImage& getDepthImage() const { return _depthImage; }
    [[nodiscard]] VkExtent2D getDrawExtent() const { return _drawExtent; }
    [[nodiscard]] VkFence getImmediateFence() const { return _immFence; }
    [[nodiscard]] VkCommandPool getImmediateCommandPool() const { return _immCommandPool; }
    [[nodiscard]] VkCommandBuffer getImmediateCommandBuffer() const { return _immCommandBuffer; }

  private:
    VulkanDevice& _device;
    AllocatedImage _drawImage;
    AllocatedImage _depthImage;
    VkExtent2D _drawExtent;

    // Immediate submit structures
    VkFence _immFence;
    VkCommandPool _immCommandPool;
    VkCommandBuffer _immCommandBuffer;

    DeletionQueue _deletionQueue;
};

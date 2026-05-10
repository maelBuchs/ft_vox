#include "RenderContext.hpp"

#include <iostream>
#include <stdexcept>

#include "../Core/VulkanDevice.hpp"

RenderContext::RenderContext(VulkanDevice& device) : _device(device), _drawExtent{} {
    createImmediateSubmitStructures();
}

RenderContext::~RenderContext() {
    _deletionQueue.flush();
}

void RenderContext::createDrawImages(VkExtent2D extent) {
    for (int i = 0; i < 2; ++i) {
        // Setup draw image
        _drawImages[i].extent = {.width = extent.width, .height = extent.height, .depth = 1};
        _drawImages[i].format = VK_FORMAT_R16G16B16A16_SFLOAT;

        VkImageUsageFlags drawImageUsages{VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};

        VkImageCreateInfo rimg_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = _drawImages[i].format,
                                    .extent = _drawImages[i].extent,
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = drawImageUsages};

        VmaAllocationCreateInfo rimg_allocinfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = 0};

        VkResult ret = vmaCreateImage(_device.getAllocator(), &rimg_info, &rimg_allocinfo,
                                      &_drawImages[i].image, &_drawImages[i].allocation, nullptr);
        if (ret != VK_SUCCESS) {
            throw std::runtime_error("Failed to create draw image");
        }

        VkImageViewCreateInfo rview_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = 0,
                                         .image = _drawImages[i].image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = _drawImages[i].format,
                                         .subresourceRange = {
                                             .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                             .baseMipLevel = 0,
                                             .levelCount = 1,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1,
                                         }};

        ret = vkCreateImageView(_device.getDevice(), &rview_info, nullptr, &_drawImages[i].imageView);
        if (ret != VK_SUCCESS) {
            throw std::runtime_error("Failed to create draw image view");
        }
        _drawImages[i].currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Setup depth image
        _depthImages[i].format = VK_FORMAT_D32_SFLOAT;
        _depthImages[i].extent = _drawImages[i].extent;

        VkImageUsageFlags depthImageUsages = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VkImageCreateInfo dimg_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = _depthImages[i].format,
                                    .extent = _depthImages[i].extent,
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = depthImageUsages};

        ret = vmaCreateImage(_device.getAllocator(), &dimg_info, &rimg_allocinfo, &_depthImages[i].image,
                             &_depthImages[i].allocation, nullptr);
        if (ret != VK_SUCCESS) {
            throw std::runtime_error("Failed to create depth image");
        }

        VkImageViewCreateInfo dview_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = 0,
                                         .image = _depthImages[i].image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = _depthImages[i].format,
                                         .subresourceRange = {
                                             .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                             .baseMipLevel = 0,
                                             .levelCount = 1,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1,
                                         }};

        ret = vkCreateImageView(_device.getDevice(), &dview_info, nullptr, &_depthImages[i].imageView);
        if (ret != VK_SUCCESS) {
            throw std::runtime_error("Failed to create depth image view");
        }
        _depthImages[i].currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    _drawExtent = extent;
}

void RenderContext::destroyDrawImages() {
    for (int i = 0; i < 2; ++i) {
        vkDestroyImageView(_device.getDevice(), _drawImages[i].imageView, nullptr);
        vmaDestroyImage(_device.getAllocator(), _drawImages[i].image, _drawImages[i].allocation);
        vkDestroyImageView(_device.getDevice(), _depthImages[i].imageView, nullptr);
        vmaDestroyImage(_device.getAllocator(), _depthImages[i].image, _depthImages[i].allocation);
    }
}

void RenderContext::createImmediateSubmitStructures() {
    VkCommandPoolCreateInfo immCommandPoolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = _device.getGraphicsQueueFamily()};

    if (vkCreateCommandPool(_device.getDevice(), &immCommandPoolInfo, nullptr, &_immCommandPool) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create immediate command pool");
    }

    VkCommandBufferAllocateInfo immCmdAllocInfo{.sType =
                                                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .pNext = nullptr,
                                                .commandPool = _immCommandPool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};

    if (vkAllocateCommandBuffers(_device.getDevice(), &immCmdAllocInfo, &_immCommandBuffer) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate immediate command buffer");
    }

    VkFenceCreateInfo immFenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};

    if (vkCreateFence(_device.getDevice(), &immFenceCreateInfo, nullptr, &_immFence) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create immediate fence");
    }

    _deletionQueue.push([this]() {
        vkDestroyFence(_device.getDevice(), _immFence, nullptr);
        vkDestroyCommandPool(_device.getDevice(), _immCommandPool, nullptr);
    });
}

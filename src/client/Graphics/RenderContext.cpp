#include "RenderContext.hpp"

#include <stdexcept>

#include "VulkanDevice.hpp"

RenderContext::RenderContext(VulkanDevice& device) : _device(device), _drawExtent{} {
    createImmediateSubmitStructures();
}

RenderContext::~RenderContext() {
    _deletionQueue.flush();
}

void RenderContext::createDrawImages(VkExtent2D extent) {
    // Setup draw image
    _drawImage.extent = {.width = extent.width, .height = extent.height, .depth = 1};
    _drawImage.format = VK_FORMAT_R16G16B16A16_SFLOAT;

    VkImageUsageFlags drawImageUsages{VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};

    VkImageCreateInfo rimg_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = _drawImage.format,
                                .extent = _drawImage.extent,
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = drawImageUsages};

    VmaAllocationCreateInfo rimg_allocinfo{
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};

    VkResult ret = vmaCreateImage(_device.getAllocator(), &rimg_info, &rimg_allocinfo,
                                  &_drawImage.image, &_drawImage.allocation, nullptr);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create draw image");
    }

    VkImageViewCreateInfo rview_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .image = _drawImage.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = _drawImage.format,
                                     .subresourceRange = {
                                         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                         .baseMipLevel = 0,
                                         .levelCount = 1,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1,
                                     }};

    ret = vkCreateImageView(_device.getDevice(), &rview_info, nullptr, &_drawImage.imageView);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create draw image view");
    }

    // Setup depth image
    _depthImage.format = VK_FORMAT_D32_SFLOAT;
    _depthImage.extent = _drawImage.extent;

    VkImageUsageFlags depthImageUsages = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    VkImageCreateInfo dimg_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = _depthImage.format,
                                .extent = _depthImage.extent,
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = depthImageUsages};

    ret = vmaCreateImage(_device.getAllocator(), &dimg_info, &rimg_allocinfo, &_depthImage.image,
                         &_depthImage.allocation, nullptr);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image");
    }

    VkImageViewCreateInfo dview_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .image = _depthImage.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = _depthImage.format,
                                     .subresourceRange = {
                                         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                         .baseMipLevel = 0,
                                         .levelCount = 1,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1,
                                     }};

    ret = vkCreateImageView(_device.getDevice(), &dview_info, nullptr, &_depthImage.imageView);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image view");
    }

    _drawExtent = extent;
}

void RenderContext::destroyDrawImages() {
    vkDestroyImageView(_device.getDevice(), _drawImage.imageView, nullptr);
    vmaDestroyImage(_device.getAllocator(), _drawImage.image, _drawImage.allocation);
    vkDestroyImageView(_device.getDevice(), _depthImage.imageView, nullptr);
    vmaDestroyImage(_device.getAllocator(), _depthImage.image, _depthImage.allocation);
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

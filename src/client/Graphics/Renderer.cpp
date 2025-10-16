#include "Renderer.hpp"

#include <cmath>
#include <iostream>
#include <memory>

#include <vulkan/vulkan.h>

#include "../Core/Window.hpp"
#include "Pipeline.hpp"
#include "VulkanDevice.hpp"
#include "VulkanSwapchain.hpp"

Renderer::Renderer(Window& window, VulkanDevice& device)
    : _window(window), _device(device), _frameNumber(0) {
    try {
        _swapchain = std::make_unique<VulkanSwapchain>(window, device);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to create VulkanSwapchain: " << e.what() << "\n";
        throw;
    }
    VkExtent2D swapchainExtent = _swapchain->getSwapchainExtent();
    _drawImage.extent = {
        .width = swapchainExtent.width, .height = swapchainExtent.height, .depth = 1};
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
    checkVkResult(ret, "Failed to create draw image");

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
    checkVkResult(ret, "Failed to create draw image view");

    // Register draw image cleanup in main deletion queue
    _mainDeletionQueue.push(
        [this]() { vkDestroyImageView(_device.getDevice(), _drawImage.imageView, nullptr); });
    _mainDeletionQueue.push([this]() {
        vmaDestroyImage(_device.getAllocator(), _drawImage.image, _drawImage.allocation);
    });

    createFrameCommandPools();
    createFrameSyncStructures();

    std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 1}};

    _globalDescriptorAllocator = std::make_unique<DescriptorAllocator>(_device, 10, sizes);
    _mainDeletionQueue.push([&]() {
        vkDestroyDescriptorPool(_device.getDevice(), _globalDescriptorAllocator->getPool(),
                                nullptr);
    });

    _gradientPipeline = std::make_unique<Pipeline>(_device, "shaders/gradient.comp.spv");

    VkDescriptorSetLayout layout = _gradientPipeline->getDescriptorSetLayout();
    _drawImageDescriptorSet = _globalDescriptorAllocator->allocate(_device, layout);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = _drawImage.imageView;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = nullptr;

    write.dstBinding = 0;
    write.dstSet = _drawImageDescriptorSet;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(_device.getDevice(), 1, &write, 0, nullptr);
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(_device.getDevice());
    _mainDeletionQueue.flush();
}

void Renderer::draw() {
    // Wait for the previous frame to finish
    VkResult ret = vkWaitForFences(_device.getDevice(), 1, &getCurrentFrame()._renderFence, VK_TRUE,
                                   VULKAN_TIMEOUT_NS);
    checkVkResult(ret, "Failed to wait for fence");

    getCurrentFrame()._deletionQueue.flush();

    ret = vkResetFences(_device.getDevice(), 1, &getCurrentFrame()._renderFence);
    checkVkResult(ret, "Failed to reset fence");

    _drawExtent.width = _drawImage.extent.width;
    _drawExtent.height = _drawImage.extent.height;

    // Acquire swapchain image
    uint32_t swapchainImageIndex = 0;
    ret =
        vkAcquireNextImageKHR(_device.getDevice(), _swapchain->getSwapchain(), VULKAN_TIMEOUT_NS,
                              getCurrentFrame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
    checkVkResult(ret, "Failed to acquire next image");

    // Reset and begin command buffer
    VkCommandBuffer commandBuffer = getCurrentFrame()._mainCommandBuffer;
    ret = vkResetCommandBuffer(commandBuffer, 0);
    checkVkResult(ret, "Failed to reset command buffer");

    VkCommandBufferBeginInfo cmdBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .pNext = nullptr,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                          .pInheritanceInfo = nullptr};

    ret = vkBeginCommandBuffer(commandBuffer, &cmdBeginInfo);
    checkVkResult(ret, "Failed to begin command buffer");

    // Transition swapchain image to GENERAL layout for clearing
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL);

    // Clear the swapchain image with an animated color
    // float flash = std::abs(std::sin(static_cast<float>(_frameNumber) / 120.0F));
    // VkClearValue clearValue{.color = {{0.0F, 0.0F, flash, 1.0F}}};

    // VkImageSubresourceRange clearRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    //                                    .baseMipLevel = 0,
    //                                    .levelCount = VK_REMAINING_MIP_LEVELS,
    //                                    .baseArrayLayer = 0,
    //                                    .layerCount = VK_REMAINING_ARRAY_LAYERS};

    // vkCmdClearColorImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL,
    //                      &clearValue.color, 1, &clearRange);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      _gradientPipeline->getPipeline());

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            _gradientPipeline->getLayout(), 0, 1, &_drawImageDescriptorSet, 0,
                            nullptr);

    vkCmdDispatch(commandBuffer, static_cast<uint32_t>(std::ceil(_drawExtent.width / 16.0)),
                  static_cast<uint32_t>(std::ceil(_drawExtent.height / 16.0)), 1);

    // Transition swapchain image to PRESENT_SRC layout
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage swapchainImage = _swapchain->getSwapchainImages().at(swapchainImageIndex);
    transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copy_image_to_image(commandBuffer, _drawImage.image, swapchainImage,
                        {_drawImage.extent.width, _drawImage.extent.height},
                        _swapchain->getSwapchainExtent());

    transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    ret = vkEndCommandBuffer(commandBuffer);
    checkVkResult(ret, "Failed to end command buffer");

    // Submit command buffer to graphics queue
    VkCommandBufferSubmitInfo cmdinfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                      .pNext = nullptr,
                                      .commandBuffer = commandBuffer,
                                      .deviceMask = 0};

    VkSemaphoreSubmitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .pNext = nullptr,
                                   .semaphore = getCurrentFrame()._swapchainSemaphore,
                                   .value = 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
                                   .deviceIndex = 0};
    VkSemaphoreSubmitInfo signalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .pNext = nullptr,
                                     .semaphore = getCurrentFrame()._renderSemaphore,
                                     .value = 1,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                     .deviceIndex = 0};

    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                         .pNext = nullptr,
                         .flags = 0,
                         .waitSemaphoreInfoCount = 1,
                         .pWaitSemaphoreInfos = &waitInfo,
                         .commandBufferInfoCount = 1,
                         .pCommandBufferInfos = &cmdinfo,
                         .signalSemaphoreInfoCount = 1,
                         .pSignalSemaphoreInfos = &signalInfo};

    ret = vkQueueSubmit2(_device.getQueue(), 1, &submit, getCurrentFrame()._renderFence);
    checkVkResult(ret, "Failed to submit to queue");

    // Present the rendered image to the screen
    VkSwapchainKHR retSwapchain = {_swapchain->getSwapchain()};
    VkPresentInfoKHR presentInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                 .pNext = nullptr,
                                 .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &getCurrentFrame()._renderSemaphore,
                                 .swapchainCount = 1,
                                 .pSwapchains = &retSwapchain,
                                 .pImageIndices = &swapchainImageIndex,
                                 .pResults = nullptr};
    ret = vkQueuePresentKHR(_device.getQueue(), &presentInfo);
    checkVkResult(ret, "Failed to present swapchain image");

    _frameNumber++;
}

void Renderer::checkVkResult(VkResult result, const char* errorMessage) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(errorMessage);
    }
}

void Renderer::createFrameCommandPools() {
    VkCommandPoolCreateInfo commandPoolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags =
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = _device.getGraphicsQueueFamily()};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult res = vkCreateCommandPool(_device.getDevice(), &commandPoolInfo, nullptr,
                                           &_frameData.at(i)._commandPool);
        checkVkResult(res, "Failed to create command pool");

        // Register command pool cleanup in main deletion queue
        _mainDeletionQueue.push([this, i]() {
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
        checkVkResult(allocRes, "Failed to allocate command buffers");
    }
}

void Renderer::createFrameSyncStructures() {
    VkFenceCreateInfo fenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    VkSemaphoreCreateInfo semaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult fenceRes = vkCreateFence(_device.getDevice(), &fenceCreateInfo, nullptr,
                                          &_frameData.at(i)._renderFence);
        checkVkResult(fenceRes, "Failed to create render fence");

        // Register fence cleanup in main deletion queue
        _mainDeletionQueue.push([this, i]() {
            vkDestroyFence(_device.getDevice(), _frameData.at(i)._renderFence, nullptr);
        });

        VkResult swapchainSemRes =
            vkCreateSemaphore(_device.getDevice(), &semaphoreCreateInfo, nullptr,
                              &_frameData.at(i)._swapchainSemaphore);
        checkVkResult(swapchainSemRes, "Failed to create swapchain semaphore");

        // Register swapchain semaphore cleanup in main deletion queue
        _mainDeletionQueue.push([this, i]() {
            vkDestroySemaphore(_device.getDevice(), _frameData.at(i)._swapchainSemaphore, nullptr);
        });

        VkResult renderSemRes = vkCreateSemaphore(_device.getDevice(), &semaphoreCreateInfo,
                                                  nullptr, &_frameData.at(i)._renderSemaphore);
        checkVkResult(renderSemRes, "Failed to create render semaphore");

        // Register render semaphore cleanup in main deletion queue
        _mainDeletionQueue.push([this, i]() {
            vkDestroySemaphore(_device.getDevice(), _frameData.at(i)._renderSemaphore, nullptr);
        });
    }
}

VkImageAspectFlags Renderer::getImageAspectMask(VkImageLayout layout) {
    return (layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageMemoryBarrier2 Renderer::createImageBarrier(VkImage image, VkImageLayout oldLayout,
                                                   VkImageLayout newLayout) const {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .pNext = nullptr,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .dstAccessMask =
                                      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                  .oldLayout = oldLayout,
                                  .newLayout = newLayout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = getImageAspectMask(newLayout),
                                                       .baseMipLevel = 0,
                                                       .levelCount = VK_REMAINING_MIP_LEVELS,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = VK_REMAINING_ARRAY_LAYERS}};

    return barrier;
}

void Renderer::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                               VkImageLayout newLayout) const {
    VkImageMemoryBarrier2 imageBarrier = createImageBarrier(image, oldLayout, newLayout);

    VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                             .pNext = nullptr,
                             .dependencyFlags = 0,
                             .memoryBarrierCount = 0,
                             .pMemoryBarriers = nullptr,
                             .bufferMemoryBarrierCount = 0,
                             .pBufferMemoryBarriers = nullptr,
                             .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &imageBarrier};

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

void Renderer::copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination,
                                   VkExtent2D srcSize, VkExtent2D dstSize) {
    VkImageBlit2 blitRegion{.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr};

    blitRegion.srcOffsets[1].x = static_cast<int32_t>(srcSize.width);
    blitRegion.srcOffsets[1].y = static_cast<int32_t>(srcSize.height);
    blitRegion.srcOffsets[1].z = 1;

    blitRegion.dstOffsets[1].x = static_cast<int32_t>(dstSize.width);
    blitRegion.dstOffsets[1].y = static_cast<int32_t>(dstSize.height);
    blitRegion.dstOffsets[1].z = 1;

    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.baseArrayLayer = 0;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcSubresource.mipLevel = 0;

    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.baseArrayLayer = 0;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstSubresource.mipLevel = 0;

    VkBlitImageInfo2 blitInfo{.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr};
    blitInfo.dstImage = destination;
    blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blitInfo.srcImage = source;
    blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blitInfo.filter = VK_FILTER_LINEAR;
    blitInfo.regionCount = 1;
    blitInfo.pRegions = &blitRegion;

    vkCmdBlitImage2(cmd, &blitInfo);
}

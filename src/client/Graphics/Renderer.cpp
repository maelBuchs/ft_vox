#include "Renderer.hpp"

#include <iostream>

#include <vulkan/vulkan.h>

#include "../Core/Window.hpp"
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

    VkCommandPoolCreateInfo commandPoolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .pNext = nullptr,
                                               .flags =
                                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                               .queueFamilyIndex = device.getGraphicsQueueFamily()};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult res = vkCreateCommandPool(_device.getDevice(), &commandPoolInfo, nullptr,
                                           &_frameData.at(i)._commandPool);
        if (res != VK_SUCCESS) {
            throw std::runtime_error("Failed to create command pool");
        }

        VkCommandBufferAllocateInfo cmdAllocInfo{.sType =
                                                     VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .pNext = nullptr,
                                                 .commandPool = _frameData.at(i)._commandPool,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};

        vkAllocateCommandBuffers(device.getDevice(), &cmdAllocInfo,
                                 &_frameData.at(i)._mainCommandBuffer);
    }

    VkFenceCreateInfo fenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    VkSemaphoreCreateInfo semaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult retFence = vkCreateFence(device.getDevice(), &fenceCreateInfo, nullptr,
                                          &_frameData.at(i)._renderFence);
        if (retFence != VK_SUCCESS) {
            throw std::runtime_error("Failed to create");
        }

        VkResult retSemaphore = vkCreateSemaphore(device.getDevice(), &semaphoreCreateInfo, nullptr,
                                                  &_frameData.at(i)._swapchainSemaphore);
        if (retSemaphore != VK_SUCCESS) {
            throw std::runtime_error("Failed to Semaphore");
        }

        retSemaphore = vkCreateSemaphore(device.getDevice(), &semaphoreCreateInfo, nullptr,
                                         &_frameData.at(i)._renderSemaphore);
        if (retSemaphore != VK_SUCCESS) {
            throw std::runtime_error("Failed to Semaphore");
        }
    }
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(_device.getDevice());

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        vkDestroyFence(_device.getDevice(), _frameData.at(i)._renderFence, nullptr);
        vkDestroySemaphore(_device.getDevice(), _frameData.at(i)._renderSemaphore, nullptr);
        vkDestroySemaphore(_device.getDevice(), _frameData.at(i)._swapchainSemaphore, nullptr);
        vkDestroyCommandPool(_device.getDevice(), _frameData.at(i)._commandPool, nullptr);
    }
}

void Renderer::draw() {
    VkResult ret = vkWaitForFences(_device.getDevice(), 1, &getCurrentFrame()._renderFence,
                                   static_cast<VkBool32>(true), 1000000000);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for fence");
    }

    ret = vkResetFences(_device.getDevice(), 1, &getCurrentFrame()._renderFence);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to reset fence");
    }

    uint32_t swapchainImageIndex = 0;
    ret =
        vkAcquireNextImageKHR(_device.getDevice(), _swapchain->getSwapchain(), 1000000000,
                              getCurrentFrame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to acquire next image");
    }

    VkCommandBuffer commandBuffer = getCurrentFrame()._mainCommandBuffer;
    ret = vkResetCommandBuffer(commandBuffer, 0);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to reset command buffer");
    }

    VkCommandBufferBeginInfo cmdBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .pNext = nullptr,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                          .pInheritanceInfo = nullptr};

    ret = vkBeginCommandBuffer(commandBuffer, &cmdBeginInfo);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin command buffer");
    }

    VkImageMemoryBarrier2 imageBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = _swapchain->getSwapchainImages().at(swapchainImageIndex)};

    VkImageAspectFlags aspectMask =
        (imageBarrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;

    imageBarrier.subresourceRange = {.aspectMask = aspectMask,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1};

    VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                             .pNext = nullptr,
                             .dependencyFlags = 0,
                             .memoryBarrierCount = 0,
                             .pMemoryBarriers = nullptr,
                             .bufferMemoryBarrierCount = 0,
                             .pBufferMemoryBarriers = nullptr,
                             .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &imageBarrier};

    vkCmdPipelineBarrier2(commandBuffer, &depInfo);

    VkClearValue clearValue;

    float flash = std::abs(std::sin(_frameNumber / 120.f));
    clearValue = {{0.0f, 0.0f, flash, 1.0f}};

    VkImageSubresourceRange clearRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = VK_REMAINING_MIP_LEVELS,
                                       .baseArrayLayer = 0,
                                       .layerCount = VK_REMAINING_ARRAY_LAYERS};

    vkCmdClearColorImage(commandBuffer, _swapchain->getSwapchainImages().at(swapchainImageIndex),
                         VK_IMAGE_LAYOUT_GENERAL, &clearValue.color, 1, &clearRange);

    VkImageMemoryBarrier2 imageBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = _swapchain->getSwapchainImages().at(swapchainImageIndex)};

    VkImageAspectFlags aspectMask2 =
        (imageBarrier2.newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;

    imageBarrier2.subresourceRange = {.aspectMask = aspectMask2,
                                      .baseMipLevel = 0,
                                      .levelCount = 1,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1};

    VkDependencyInfo depInfo2{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                              .pNext = nullptr,
                              .dependencyFlags = 0,
                              .memoryBarrierCount = 0,
                              .pMemoryBarriers = nullptr,
                              .bufferMemoryBarrierCount = 0,
                              .pBufferMemoryBarriers = nullptr,
                              .imageMemoryBarrierCount = 1,
                              .pImageMemoryBarriers = &imageBarrier2};

    vkCmdPipelineBarrier2(commandBuffer, &depInfo2);

    ret = vkEndCommandBuffer(commandBuffer);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to end command buffer");
    }

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
                         .waitSemaphoreInfoCount = &waitInfo == nullptr ? 0 : 1,
                         .pWaitSemaphoreInfos = &waitInfo,
                         .commandBufferInfoCount = 1,
                         .pCommandBufferInfos = &cmdinfo,
                         .signalSemaphoreInfoCount = &signalInfo == nullptr ? 0 : 1,
                         .pSignalSemaphoreInfos = &signalInfo};

    ret = vkQueueSubmit2(_device.getQueue(), 1, &submit, getCurrentFrame()._renderFence);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit to queue");
    }

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
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }
    _frameNumber++;
}

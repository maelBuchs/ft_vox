#include "Renderer.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>

#include <vulkan/vulkan.h>

#include "../Core/Window.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "Pipeline.hpp"
#include "Pipeline/GraphicsPipelineBuilder.hpp"
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

    // Create gradient effect
    ComputeEffect gradient;
    gradient.name = "gradient";
    gradient.pipeline = std::make_unique<Pipeline>(_device, "shaders/gradient.comp.spv");
    gradient.data.data1 = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F); // Red
    gradient.data.data2 = glm::vec4(0.0F, 0.0F, 1.0F, 1.0F); // Blue
    _backgroundEffects.push_back(std::move(gradient));

    // Create sky effect
    ComputeEffect sky;
    sky.name = "sky";
    sky.pipeline = std::make_unique<Pipeline>(_device, "shaders/sky.comp.spv");
    sky.data.data1 = glm::vec4(0.1F, 0.2F, 0.4F, 0.97F); // Sky color + star threshold
    _backgroundEffects.push_back(std::move(sky));

    VkDescriptorSetLayout layout = _backgroundEffects[0].pipeline->getDescriptorSetLayout();
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

    _mainDeletionQueue.push([this]() {
        vkDestroyFence(_device.getDevice(), _immFence, nullptr);
        vkDestroyCommandPool(_device.getDevice(), _immCommandPool, nullptr);
    });

    // Initialize triangle pipeline
    initTrianglePipeline();

    // Initialize ImGui - must be last after all Vulkan resources are ready
    initImGui();
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

    // Transition draw image to GENERAL layout for compute shader
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL);

    // Execute compute shader for background
    ComputeEffect& effect = _backgroundEffects[static_cast<size_t>(_currentBackgroundEffect)];

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      effect.pipeline->getPipeline());

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            effect.pipeline->getLayout(), 0, 1, &_drawImageDescriptorSet, 0,
                            nullptr);

    vkCmdPushConstants(commandBuffer, effect.pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ComputePushConstants), &effect.data);

    vkCmdDispatch(commandBuffer, static_cast<uint32_t>(std::ceil(_drawExtent.width / 16.0)),
                  static_cast<uint32_t>(std::ceil(_drawExtent.height / 16.0)), 1);

    // Transition draw image to COLOR_ATTACHMENT_OPTIMAL for triangle rendering
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Draw triangle geometry
    drawGeometry(commandBuffer);

    // Transition draw image to TRANSFER_SRC for copying to swapchain
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage swapchainImage = _swapchain->getSwapchainImages().at(swapchainImageIndex);
    transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copy_image_to_image(commandBuffer, _drawImage.image, swapchainImage,
                        {_drawImage.extent.width, _drawImage.extent.height},
                        _swapchain->getSwapchainExtent());

    transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = _swapchain->getSwapchainImageViews()[swapchainImageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp =
        VK_ATTACHMENT_LOAD_OP_LOAD; // On charge ce qui a déjà été dessiné (notre scène)
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = {.offset = {0, 0}, .extent = _swapchain->getSwapchainExtent()};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(commandBuffer, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);

    vkCmdEndRendering(commandBuffer);

    transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

void Renderer::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function) {
    vkResetCommandBuffer(_immCommandBuffer, 0);

    VkCommandBuffer cmd = _immCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .pNext = nullptr,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                          .pInheritanceInfo = nullptr};

    if (vkBeginCommandBuffer(cmd, &cmdBeginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin command buffer");
    }

    function(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to end command buffer");
    }

    VkCommandBufferSubmitInfo cmdinfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                      .pNext = nullptr,
                                      .commandBuffer = cmd,
                                      .deviceMask = 0};

    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                         .pNext = nullptr,
                         .flags = 0,
                         .waitSemaphoreInfoCount = 0,
                         .pWaitSemaphoreInfos = nullptr,
                         .commandBufferInfoCount = 1,
                         .pCommandBufferInfos = &cmdinfo,
                         .signalSemaphoreInfoCount = 0,
                         .pSignalSemaphoreInfos = nullptr};

    if (vkQueueSubmit2(_device.getQueue(), 1, &submit, _immFence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit to queue");
    }

    if (vkWaitForFences(_device.getDevice(), 1, &_immFence, VK_TRUE, VULKAN_TIMEOUT_NS) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for fence");
    }

    if (vkResetFences(_device.getDevice(), 1, &_immFence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to reset fence");
    }
}

void Renderer::initImGui() {
    std::array<VkDescriptorPoolSize, 11> pool_sizes = {
        {{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 1000},
         {.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1000}}};

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1000,
        .poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes)),
        .pPoolSizes = pool_sizes.data()};

    VkDescriptorPool imguiPool = VK_NULL_HANDLE;

    if (vkCreateDescriptorPool(_device.getDevice(), &pool_info, nullptr, &imguiPool) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImGui descriptor pool");
    }

    ImGui::CreateContext();

    ImGui_ImplSDL3_InitForVulkan(_window.getSDLWindow());

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = _device.getInstance();
    init_info.PhysicalDevice = _device.getPhysicalDevice();
    init_info.Device = _device.getDevice();
    init_info.QueueFamily = _device.getGraphicsQueueFamily();
    init_info.Queue = _device.getQueue();
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    init_info.UseDynamicRendering = true;

    // Configure dynamic rendering pipeline info
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    VkFormat swapchainFormat = _swapchain->getSwapchainImageFormat();
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats =
        &swapchainFormat;

    ImGui_ImplVulkan_Init(&init_info);

    // 5. Ajouter le nettoyage à la DeletionQueue
    _mainDeletionQueue.push([this, imguiPool]() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(_device.getDevice(), imguiPool, nullptr);
    });
}

void Renderer::initTrianglePipeline() {
    VkShaderModule triangleFragShader =
        Pipeline::loadShaderModule(_device, "shaders/colored_triangle.frag.spv");
    VkShaderModule triangleVertexShader =
        Pipeline::loadShaderModule(_device, "shaders/colored_triangle.vert.spv");

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .pNext = nullptr,
                                                  .flags = 0,
                                                  .setLayoutCount = 0,
                                                  .pSetLayouts = nullptr,
                                                  .pushConstantRangeCount = 0,
                                                  .pPushConstantRanges = nullptr};

    if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutInfo, nullptr,
                               &_trianglePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create triangle pipeline layout");
    }

    GraphicsPipelineBuilder pipelineBuilder;
    pipelineBuilder.setPipelineLayout(_trianglePipelineLayout);
    pipelineBuilder.setShaders(triangleVertexShader, triangleFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.disableDepthtest();
    pipelineBuilder.setColorAttachmentFormat(_drawImage.format);
    pipelineBuilder.setDepthFormat(VK_FORMAT_UNDEFINED);

    _trianglePipeline = pipelineBuilder.build(_device.getDevice());

    vkDestroyShaderModule(_device.getDevice(), triangleFragShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), triangleVertexShader, nullptr);

    _mainDeletionQueue.push([this]() {
        vkDestroyPipelineLayout(_device.getDevice(), _trianglePipelineLayout, nullptr);
        vkDestroyPipeline(_device.getDevice(), _trianglePipeline, nullptr);
    });
}

void Renderer::drawGeometry(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo colorAttachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .pNext = nullptr,
                                              .imageView = _drawImage.imageView,
                                              .imageLayout =
                                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .resolveMode = VK_RESOLVE_MODE_NONE,
                                              .resolveImageView = VK_NULL_HANDLE,
                                              .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {}};

    VkRenderingInfo renderInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .pNext = nullptr,
                               .flags = 0,
                               .renderArea = {.offset = {0, 0}, .extent = _drawExtent},
                               .layerCount = 1,
                               .viewMask = 0,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &colorAttachment,
                               .pDepthAttachment = nullptr,
                               .pStencilAttachment = nullptr};

    vkCmdBeginRendering(cmd, &renderInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);

    VkViewport viewport{.x = 0.0F,
                        .y = 0.0F,
                        .width = static_cast<float>(_drawExtent.width),
                        .height = static_cast<float>(_drawExtent.height),
                        .minDepth = 0.0F,
                        .maxDepth = 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{.offset = {0, 0}, .extent = _drawExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

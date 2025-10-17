#include "Renderer.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../Core/Window.hpp"
#include "../Game/Camera.hpp"
#include "common/World/Chunk.hpp"
#include "common/World/ChunkMesh.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "MeshManager.hpp"
#include "Pipeline/GraphicsPipelineBuilder.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanDevice.hpp"
#include "VulkanSwapchain.hpp"

Renderer::Renderer(Window& window, VulkanDevice& device, BlockRegistry& registry)
    : _window(window), _device(device), _blockRegistry(registry), _frameNumber(0) {
    try {
        _swapchain = std::make_unique<VulkanSwapchain>(window, device);
        _bufferManager = std::make_unique<VulkanBuffer>(device);
        _meshManager = std::make_unique<MeshManager>(device, *_bufferManager);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to create VulkanSwapchain: " << e.what() << "\n";
        throw;
    }

    // Create draw and depth images
    VkExtent2D swapchainExtent = _swapchain->getSwapchainExtent();
    createDrawImages(swapchainExtent);

    // Set draw extent to match swapchain
    _drawExtent = swapchainExtent;

    // Register draw and depth images cleanup in main deletion queue
    _mainDeletionQueue.push([this]() { destroyDrawImages(); });

    createFrameCommandPools();
    createFrameSyncStructures();

    // Create semaphores for each swapchain image
    size_t swapchainImageCount = _swapchain->getSwapchainImages().size();
    _swapchainSemaphores.resize(swapchainImageCount);
    _renderSemaphores.resize(swapchainImageCount);

    VkSemaphoreCreateInfo semaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};

    for (size_t i = 0; i < swapchainImageCount; i++) {
        if (vkCreateSemaphore(_device.getDevice(), &semaphoreCreateInfo, nullptr,
                              &_swapchainSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create swapchain semaphore");
        }
        if (vkCreateSemaphore(_device.getDevice(), &semaphoreCreateInfo, nullptr,
                              &_renderSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render semaphore");
        }

        _mainDeletionQueue.push([this, i]() {
            vkDestroySemaphore(_device.getDevice(), _swapchainSemaphores[i], nullptr);
            vkDestroySemaphore(_device.getDevice(), _renderSemaphores[i], nullptr);
        });
    }

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 1.0F}};

    _globalDescriptorAllocator.init(_device.getDevice(), 10, sizes);
    _mainDeletionQueue.push(
        [this]() { _globalDescriptorAllocator.destroyPools(_device.getDevice()); });

    // Initialize per-frame descriptor allocators
    for (size_t i = 0; i < FRAME_OVERLAP; i++) {
        std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes = {
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .ratio = 3.0F},
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .ratio = 4.0F},
        };

        _frameData[i]._frameDescriptors.init(_device.getDevice(), 1000, frameSizes);

        _mainDeletionQueue.push(
            [this, i]() { _frameData[i]._frameDescriptors.destroyPools(_device.getDevice()); });
    }

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

    // Initialize camera - angled view to see 3D perspective (corner view)
    _camera = std::make_unique<Camera>(glm::vec3(30.0F, 20.0F, 30.0F), -135.0F, -20.0F);

    // Initialize voxel pipeline (both filled and wireframe)
    initVoxelPipeline();

    // Create test chunk
    initTestChunk();

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
    getCurrentFrame()._frameDescriptors.clearPools(_device.getDevice());

    ret = vkResetFences(_device.getDevice(), 1, &getCurrentFrame()._renderFence);
    checkVkResult(ret, "Failed to reset fence");

    // Acquire swapchain image
    uint32_t swapchainImageIndex = 0;
    auto semaphoreIndex = static_cast<uint32_t>(_frameNumber % _swapchainSemaphores.size());
    ret =
        vkAcquireNextImageKHR(_device.getDevice(), _swapchain->getSwapchain(), VULKAN_TIMEOUT_NS,
                              _swapchainSemaphores[semaphoreIndex], nullptr, &swapchainImageIndex);
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

    // Transition draw image to COLOR_ATTACHMENT_OPTIMAL
    transitionImage(commandBuffer, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Transition depth image to DEPTH_ATTACHMENT_OPTIMAL
    // Note: Only transition from UNDEFINED on first use, afterwards it stays in
    // DEPTH_ATTACHMENT_OPTIMAL
    static bool firstFrame = true;
    if (firstFrame) {
        transitionImage(commandBuffer, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        firstFrame = false;
    }
    // Depth image stays in DEPTH_ATTACHMENT_OPTIMAL between frames

    // Render voxel geometry
    drawVoxels(commandBuffer);

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
                                   .semaphore = _swapchainSemaphores[semaphoreIndex],
                                   .value = 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
                                   .deviceIndex = 0};
    VkSemaphoreSubmitInfo signalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .pNext = nullptr,
                                     .semaphore = _renderSemaphores[semaphoreIndex],
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
                                 .pWaitSemaphores = &_renderSemaphores[semaphoreIndex],
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

    for (uint64_t i = 0; i < FRAME_OVERLAP; i++) {
        VkResult fenceRes = vkCreateFence(_device.getDevice(), &fenceCreateInfo, nullptr,
                                          &_frameData.at(i)._renderFence);
        checkVkResult(fenceRes, "Failed to create render fence");

        // Register fence cleanup in main deletion queue
        _mainDeletionQueue.push([this, i]() {
            vkDestroyFence(_device.getDevice(), _frameData.at(i)._renderFence, nullptr);
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

    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    VkFormat swapchainFormat = _swapchain->getSwapchainImageFormat();
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats =
        &swapchainFormat;

    ImGui_ImplVulkan_Init(&init_info);

    _mainDeletionQueue.push([this, imguiPool]() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(_device.getDevice(), imguiPool, nullptr);
    });
}

void Renderer::createDrawImages(VkExtent2D extent) {
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
    checkVkResult(ret, "Failed to create depth image");

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
    checkVkResult(ret, "Failed to create depth image view");
}

void Renderer::destroyDrawImages() {
    vkDestroyImageView(_device.getDevice(), _drawImage.imageView, nullptr);
    vmaDestroyImage(_device.getAllocator(), _drawImage.image, _drawImage.allocation);
    vkDestroyImageView(_device.getDevice(), _depthImage.imageView, nullptr);
    vmaDestroyImage(_device.getAllocator(), _depthImage.image, _depthImage.allocation);
}

void Renderer::resizeSwapchain() {
    // Wait for all GPU operations to complete
    vkDeviceWaitIdle(_device.getDevice());

    // Destroy old images
    destroyDrawImages();

    // Destroy old swapchain
    _swapchain.reset();

    // Recreate swapchain with new size
    _swapchain = std::make_unique<VulkanSwapchain>(_window, _device);

    // Recreate draw and depth images with new size
    VkExtent2D newExtent = _swapchain->getSwapchainExtent();
    createDrawImages(newExtent);

    // Update draw extent to match new size
    _drawExtent = newExtent;
}

void Renderer::initVoxelPipeline() {
    VkShaderModule voxelFragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");
    VkShaderModule voxelVertexShader =
        Pipeline::loadShaderModule(_device, "shaders/voxel.vert.spv");

    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(ChunkPushConstants)};

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .pNext = nullptr,
                                                  .flags = 0,
                                                  .setLayoutCount = 0,
                                                  .pSetLayouts = nullptr,
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &pushConstantRange};

    VkPipelineLayout voxelPipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutInfo, nullptr,
                               &voxelPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create voxel pipeline layout");
    }

    VkVertexInputBindingDescription binding{
        .binding = 0, .stride = sizeof(VoxelVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

    std::vector<VkVertexInputAttributeDescription> attributes{
        {.location = 0,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(VoxelVertex, position)},
        {.location = 1,
         .binding = 0,
         .format = VK_FORMAT_R32_SFLOAT,
         .offset = offsetof(VoxelVertex, uv_x)},
        {.location = 2,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(VoxelVertex, normal)},
        {.location = 3,
         .binding = 0,
         .format = VK_FORMAT_R32_SFLOAT,
         .offset = offsetof(VoxelVertex, uv_y)},
        {.location = 4,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .offset = offsetof(VoxelVertex, color)}};

    // Create FILLED pipeline
    GraphicsPipelineBuilder pipelineBuilder;
    pipelineBuilder.setPipelineLayout(voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(_drawImage.format);
    pipelineBuilder.setDepthFormat(_depthImage.format);
    pipelineBuilder.setVertexInputState({binding}, attributes);

    VkPipeline voxelPipeline = pipelineBuilder.build(_device.getDevice());
    _voxelPipeline.init(voxelPipeline, voxelPipelineLayout);

    // Create WIREFRAME pipeline
    pipelineBuilder.clear();
    pipelineBuilder.setPipelineLayout(voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_LINE); // WIREFRAME MODE
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(_drawImage.format);
    pipelineBuilder.setDepthFormat(_depthImage.format);

    pipelineBuilder.setVertexInputState({binding}, attributes);

    VkPipeline voxelWireframePipeline = pipelineBuilder.build(_device.getDevice());

    // Both pipelines share the same layout
    _voxelWireframePipeline.init(voxelWireframePipeline, voxelPipelineLayout);

    vkDestroyShaderModule(_device.getDevice(), voxelFragShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), voxelVertexShader, nullptr);

    // Register cleanup for both pipelines
    _mainDeletionQueue.push([this]() {
        _voxelPipeline.cleanup(_device);
        // Only destroy the pipeline object, not the layout (already cleaned up above)
        if (_voxelWireframePipeline.getPipeline() != VK_NULL_HANDLE) {
            vkDestroyPipeline(_device.getDevice(), _voxelWireframePipeline.getPipeline(), nullptr);
        }
    });
}

void Renderer::initTestChunk() {
    _testChunk = std::make_unique<Chunk>();

    // Create a flat terrain with some variation with stone : 1, grass_block : 2, oak_wood : 3 ,
    // water : 4
    for (int x = 0; x < Chunk::CHUNK_SIZE; x++) {
        for (int z = 0; z < Chunk::CHUNK_SIZE; z++) {
            int height = 4 + (std::rand() % 5); // Random height between 4 and 8
            for (int y = 0; y < Chunk::CHUNK_SIZE; y++) {
                if (y < height - 1) {
                    _testChunk->setBlock(x, y, z, 1); // stone
                } else if (y == height - 1) {
                    _testChunk->setBlock(x, y, z, 2); // grass_block
                } else if (y < 4) {
                    _testChunk->setBlock(x, y, z, 4); // water
                } else if (y == height && (x + z) % 5 == 0) {
                    _testChunk->setBlock(x, y, z, 3); // oak_wood (some trees)
                } else {
                    _testChunk->setBlock(x, y, z, 0); // air
                }
            }
        }
    }

    // Generate mesh
    std::vector<VoxelVertex> vertices;
    std::vector<uint32_t> indices;
    ChunkMesh::generateMesh(*_testChunk, _blockRegistry, vertices, indices);
    std::cout << "[RENDERER] Chunk mesh: " << vertices.size() << " vertices, " << indices.size()
              << " indices\n";

    // Upload to GPU - cast VoxelVertex to Vertex span
    std::vector<Vertex> convertedVertices;
    convertedVertices.reserve(vertices.size());
    for (const auto& vv : vertices) {
        convertedVertices.push_back(Vertex{.position = vv.position,
                                           .uv_x = vv.uv_x,
                                           .normal = vv.normal,
                                           .uv_y = vv.uv_y,
                                           .color = vv.color});
    }
    _testChunkMesh = _meshManager->uploadMesh(
        indices, convertedVertices,
        [this](std::function<void(VkCommandBuffer)>&& func) { immediateSubmit(std::move(func)); });

    _mainDeletionQueue.push([this]() { _meshManager->destroyMesh(_testChunkMesh); });
}

void Renderer::drawVoxels(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = _drawImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {0.1F, 0.2F, 0.3F, 1.0F}}}};

    VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = _depthImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {.depth = 1.0F, .stencil = 0}}};

    VkRenderingInfo renderInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .pNext = nullptr,
                               .flags = 0,
                               .renderArea = {.offset = {0, 0}, .extent = _drawExtent},
                               .layerCount = 1,
                               .viewMask = 0,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &colorAttachment,
                               .pDepthAttachment = &depthAttachment,
                               .pStencilAttachment = nullptr};

    vkCmdBeginRendering(cmd, &renderInfo);

    // Set viewport and scissor
    VkViewport viewport{.x = 0.0F,
                        .y = 0.0F,
                        .width = static_cast<float>(_drawExtent.width),
                        .height = static_cast<float>(_drawExtent.height),
                        .minDepth = 0.0F,
                        .maxDepth = 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{.offset = {0, 0}, .extent = _drawExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind pipeline based on wireframe mode
    VkPipeline activePipeline =
        _wireframeMode ? _voxelWireframePipeline.getPipeline() : _voxelPipeline.getPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    // Set up view-projection matrix
    glm::mat4 view = _camera->getViewMatrix();

    // Standard perspective projection for Vulkan
    glm::mat4 projection = glm::perspective(
        glm::radians(80.0F),
        static_cast<float>(_drawExtent.width) / static_cast<float>(_drawExtent.height),
        0.1F,    // near plane
        10000.0F // far plane
    );
    projection[1][1] *= -1.0F; // Flip Y for Vulkan coordinate system

    glm::mat4 viewProjection = projection * view;

    // Set push constants
    ChunkPushConstants pushConstants{};
    pushConstants.viewProjection = viewProjection;
    pushConstants.chunkWorldPos = glm::vec3(0.0F, 0.0F, 0.0F);

    vkCmdPushConstants(cmd, _voxelPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(ChunkPushConstants), &pushConstants);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {_testChunkMesh.vertexBuffer.buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, _testChunkMesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Get index count from buffer size
    uint32_t indexCount =
        static_cast<uint32_t>(_testChunkMesh.indexBuffer.info.size / sizeof(uint32_t));

    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);

    vkCmdEndRendering(cmd);
}

void Renderer::updateFPS(float deltaTime) {
    _frameTimeAccumulator += deltaTime;
    _frameCount++;

    // Update FPS every 0.5 seconds
    if (_frameTimeAccumulator >= 0.5F) {
        _fps = static_cast<float>(_frameCount) / _frameTimeAccumulator;
        _frameTimeAccumulator = 0.0F;
        _frameCount = 0;
    }
}

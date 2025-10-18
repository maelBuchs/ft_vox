#include "Renderer.hpp"

#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "../Core/Window.hpp"
#include "../Game/Camera.hpp"
#include "common/World/Chunk.hpp"
#include "Core/VulkanBuffer.hpp"
#include "Core/VulkanDevice.hpp"
#include "Core/VulkanSwapchain.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "Rendering/CommandExecutor.hpp"
#include "Rendering/FrameManager.hpp"
#include "Rendering/RenderContext.hpp"
#include "Voxel/MeshManager.hpp"
#include "Voxel/VoxelRenderer.hpp"

// tmp ChunkInstanciator

ChunkInstanciator* chunkInstanciator = nullptr;

Renderer::Renderer(Window& window, VulkanDevice& device, BlockRegistry& registry)
    : _window(window), _device(device), _blockRegistry(registry) {
    try {
        _swapchain = std::make_unique<VulkanSwapchain>(window, device);
        _bufferManager = std::make_unique<VulkanBuffer>(device);
        _meshManager = std::make_unique<MeshManager>(device, *_bufferManager);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to create VulkanSwapchain: " << e.what() << "\n";
        throw;
    }

    // Initialize new class compositions (Phase 3 refactor)
    _frameManager = std::make_unique<FrameManager>(device);
    _renderContext = std::make_unique<RenderContext>(device);
    _commandExecutor = std::make_unique<CommandExecutor>(device, *_renderContext);

    // Create draw and depth images
    VkExtent2D swapchainExtent = _swapchain->getSwapchainExtent();
    _renderContext->createDrawImages(swapchainExtent);

    // Register draw and depth images cleanup in main deletion queue
    _mainDeletionQueue.push([this]() { _renderContext->destroyDrawImages(); });

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
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 1.0F},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .ratio = 1.0F}};

    _globalDescriptorAllocator.init(_device.getDevice(), 10, sizes);
    _mainDeletionQueue.push(
        [this]() { _globalDescriptorAllocator.destroyPools(_device.getDevice()); });

    // Initialize camera - angled view to see 3D perspective (corner view)
    _camera = std::make_unique<Camera>(glm::vec3(30.0F, 70.0F, 30.0F), -135.0F, -20.0F);

    // Initialize voxel renderer
    _voxelRenderer = std::make_unique<VoxelRenderer>(device, *_meshManager, registry,
                                                     *_renderContext, *_commandExecutor,
                                                     *_bufferManager, _globalDescriptorAllocator);
    _voxelRenderer->initPipelines();
    _voxelRenderer->initTestChunk();
    chunkInstanciator = new ChunkInstanciator();

    // Initialize ImGui - must be last after all Vulkan resources are ready
    initImGui();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(_device.getDevice());

    _mainDeletionQueue.flush();
    // Destroy managed objects first (in reverse order of creation)
    // This ensures their internal deletion queues are flushed before the main queue
    _voxelRenderer.reset();
    _commandExecutor.reset();
    _renderContext.reset();
    _frameManager.reset();
}

void Renderer::draw() {

    // Get current frame from FrameManager
    auto& currentFrame = _frameManager->getCurrentFrame();

    // Wait for the previous frame to finish
    VkResult ret = vkWaitForFences(_device.getDevice(), 1, &currentFrame._renderFence, VK_TRUE,
                                   VULKAN_TIMEOUT_NS);
    checkVkResult(ret, "Failed to wait for fence");

    currentFrame._deletionQueue.flush();
    currentFrame._frameDescriptors.clearPools(_device.getDevice());

    ret = vkResetFences(_device.getDevice(), 1, &currentFrame._renderFence);
    checkVkResult(ret, "Failed to reset fence");

    // Acquire swapchain image
    uint32_t swapchainImageIndex = 0;
    auto semaphoreIndex =
        static_cast<uint32_t>(_frameManager->getFrameNumber() % _swapchainSemaphores.size());
    ret =
        vkAcquireNextImageKHR(_device.getDevice(), _swapchain->getSwapchain(), VULKAN_TIMEOUT_NS,
                              _swapchainSemaphores[semaphoreIndex], nullptr, &swapchainImageIndex);
    checkVkResult(ret, "Failed to acquire next image");

    // Reset and begin command buffer
    VkCommandBuffer commandBuffer = currentFrame._mainCommandBuffer;
    ret = vkResetCommandBuffer(commandBuffer, 0);
    checkVkResult(ret, "Failed to reset command buffer");

    VkCommandBufferBeginInfo cmdBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .pNext = nullptr,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                          .pInheritanceInfo = nullptr};

    ret = vkBeginCommandBuffer(commandBuffer, &cmdBeginInfo);
    checkVkResult(ret, "Failed to begin command buffer");

    // Get images from RenderContext
    const RenderContext::AllocatedImage& drawImage = _renderContext->getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _renderContext->getDepthImage();

    // Transition draw image to COLOR_ATTACHMENT_OPTIMAL
    _commandExecutor->transitionImage(commandBuffer, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Transition depth image to DEPTH_ATTACHMENT_OPTIMAL
    static bool firstFrame = true;
    if (firstFrame) {
        _commandExecutor->transitionImage(commandBuffer, depthImage.image,
                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        firstFrame = false;
    }
    // ici

    chunkInstanciator->updateChunksAroundPlayer(_camera->getPosition().x, _camera->getPosition().y,
                                                _camera->getPosition().z, 12);

    // Render voxel geometry using VoxelRenderer
    _voxelRenderer->drawVoxels(commandBuffer, *_camera, _wireframeMode);

    // Transition draw image to TRANSFER_SRC for copying to swapchain
    _commandExecutor->transitionImage(commandBuffer, drawImage.image,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage swapchainImage = _swapchain->getSwapchainImages().at(swapchainImageIndex);
    _commandExecutor->transitionImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    _commandExecutor->copyImageToImage(commandBuffer, drawImage.image, swapchainImage,
                                       {drawImage.extent.width, drawImage.extent.height},
                                       _swapchain->getSwapchainExtent());

    _commandExecutor->transitionImage(commandBuffer, swapchainImage,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = _swapchain->getSwapchainImageViews()[swapchainImageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp =
        VK_ATTACHMENT_LOAD_OP_LOAD; // Load what has already been drawn (our scene)
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

    _commandExecutor->transitionImage(commandBuffer, swapchainImage,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

    ret = vkQueueSubmit2(_device.getQueue(), 1, &submit, currentFrame._renderFence);
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

    _frameManager->incrementFrame();
}

void Renderer::checkVkResult(VkResult result, const char* errorMessage) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(errorMessage);
    }
}

void Renderer::resizeSwapchain() {
    // Wait for all GPU operations to complete
    vkDeviceWaitIdle(_device.getDevice());

    // Destroy old images
    _renderContext->destroyDrawImages();

    // Destroy old swapchain
    _swapchain.reset();

    // Recreate swapchain with new size
    _swapchain = std::make_unique<VulkanSwapchain>(_window, _device);

    // Recreate draw and depth images with new size
    VkExtent2D newExtent = _swapchain->getSwapchainExtent();
    _renderContext->createDrawImages(newExtent);
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

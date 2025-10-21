#include "Renderer.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "../Core/Window.hpp"
#include "../Game/Camera.hpp"
#include "common/World/BlockRegistry.hpp"
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
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .ratio = 1.0F},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .ratio = 1.0F}};

    _globalDescriptorAllocator.init(_device.getDevice(), 10, sizes);
    _mainDeletionQueue.push(
        [this]() { _globalDescriptorAllocator.destroyPools(_device.getDevice()); });

    _camera = std::make_unique<Camera>(glm::vec3(30.0F, 70.0F, 30.0F), -135.0F, -20.0F);

    loadTextureAtlas();
    loadBlueNoiseTexture();

    // Initialize voxel renderer
    _voxelRenderer = std::make_unique<VoxelRenderer>(
        device, *_meshManager, registry, *_renderContext, *_commandExecutor, *_bufferManager,
        _globalDescriptorAllocator, *this);
    _voxelRenderer->initPipelines(_textureAtlas.imageView, _textureAtlasSampler);
    // _voxelRenderer->initTestChunk();
    chunkInstanciator = new ChunkInstanciator();

    // Initialize sky rendering pipeline
    initSkyPipeline();

    initImGui();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(_device.getDevice());

    if (_textureAtlasSampler != VK_NULL_HANDLE) {
        vkDestroySampler(_device.getDevice(), _textureAtlasSampler, nullptr);
    }
    if (_textureAtlas.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(_device.getDevice(), _textureAtlas.imageView, nullptr);
    }
    if (_textureAtlas.image != VK_NULL_HANDLE) {
        vmaDestroyImage(_device.getAllocator(), _textureAtlas.image, _textureAtlas.allocation);
    }

    _mainDeletionQueue.flush();
    // Destroy managed objects first (in reverse order of creation)
    // This ensures their internal deletion queues are flushed before the main queue
    _voxelRenderer.reset();
    _commandExecutor.reset();
    _renderContext.reset();
    _frameManager.reset();
}

void Renderer::draw(float timeOfDay) {

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

    chunkInstanciator->updateChunksAroundPlayer(_camera->getPosition());

    // Render sky first (will fill the background)
    drawSky(commandBuffer, timeOfDay);

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

uint32_t Renderer::getTextureId(const std::string& path) {
    if (path.empty())
        return 0; // Return 0 for air or invalid textures

    auto it = _texturePathToId.find(path);
    if (it != _texturePathToId.end()) {
        return it->second;
    }

    // This case should ideally not be hit if pre-loading is done correctly
    uint32_t id = _nextTextureId++;
    _texturePathToId[path] = id;
    return id;
}

void Renderer::loadTextureAtlas() {
    std::cout << "Loading texture atlas...\n";

    // 1. Discover all unique texture paths from the BlockRegistry
    for (int i = 0; i < MAX_BLOCKS; ++i) {
        std::string path;
        path = _blockRegistry.getTexturePath(i, "all");
        if (!path.empty())
            (void)getTextureId(path); // Capture ID to populate map
        path = _blockRegistry.getTexturePath(i, "top");
        if (!path.empty())
            (void)getTextureId(path);
        path = _blockRegistry.getTexturePath(i, "bottom");
        if (!path.empty())
            (void)getTextureId(path);
        path = _blockRegistry.getTexturePath(i, "side");
        if (!path.empty())
            (void)getTextureId(path);
    }

    // Create sorted list for deterministic layout
    std::map<uint32_t, std::string> idToPath;
    for (const auto& [path, id] : _texturePathToId) {
        idToPath[id] = path;
    }

    std::vector<std::string> uniqueTexturePaths;
    for (const auto& [id, path] : idToPath) {
        uniqueTexturePaths.push_back(path);
    }

    if (uniqueTexturePaths.empty()) {
        throw std::runtime_error("No textures found to build atlas.");
    }

    std::cout << "Found " << uniqueTexturePaths.size() << " unique textures\n";

    // 2. Load all images into CPU memory using stb_image
    struct ImageData {
        int width, height, channels;
        stbi_uc* pixels;
    };

    // Flip source images vertically so UV origin (0,0) = block bottom-left.
    stbi_set_flip_vertically_on_load(1);

    std::vector<ImageData> loadedImages;
    int atlasDimension = 0;

    for (const auto& path : uniqueTexturePaths) {
        ImageData img;
        img.pixels =
            stbi_load(path.c_str(), &img.width, &img.height, &img.channels, STBI_rgb_alpha);
        if (!img.pixels) {
            throw std::runtime_error("Failed to load texture file: " + path);
        }
        if (atlasDimension == 0)
            atlasDimension = img.width;
        if (img.width != atlasDimension || img.height != atlasDimension) {
            stbi_image_free(img.pixels);
            throw std::runtime_error("All block textures must be square and same dimensions! " +
                                     path);
        }
        std::cout << "  Loaded: " << path << " (" << img.width << "x" << img.height << ")\n";
        loadedImages.push_back(img);
    }

    // Reset flip flag for any subsequent texture loads elsewhere in the codebase.
    stbi_set_flip_vertically_on_load(0);

    // 3. Determine atlas size and create staging buffer
    _atlasTexturesPerRow = static_cast<int>(std::ceil(std::sqrt(loadedImages.size())));
    const int atlasWidth = _atlasTexturesPerRow * atlasDimension;
    const int atlasHeight = _atlasTexturesPerRow * atlasDimension;
    const VkDeviceSize atlasTotalSize = static_cast<VkDeviceSize>(atlasWidth) * atlasHeight * 4;

    std::cout << "Creating atlas: " << atlasWidth << "x" << atlasHeight << " ("
              << _atlasTexturesPerRow << "x" << _atlasTexturesPerRow << " tiles)\n";

    AllocatedBuffer stagingBuffer = _bufferManager->createStagingBuffer(atlasTotalSize);

    // 4. Copy individual image data into the staging buffer in a grid layout
    for (size_t i = 0; i < loadedImages.size(); ++i) {
        int tileX = static_cast<int>(i) % _atlasTexturesPerRow;
        int tileY = static_cast<int>(i) / _atlasTexturesPerRow;
        char* startPtr = static_cast<char*>(stagingBuffer.info.pMappedData) +
                         (tileY * atlasWidth * atlasDimension * 4) + (tileX * atlasDimension * 4);

        for (int row = 0; row < atlasDimension; ++row) {
            memcpy(startPtr + (row * atlasWidth * 4),
                   loadedImages[i].pixels + (row * atlasDimension * 4), atlasDimension * 4);
        }
    }

    // 5. Create the final GPU image (the atlas)
    _textureAtlas.extent = {static_cast<uint32_t>(atlasWidth), static_cast<uint32_t>(atlasHeight),
                            1};
    _textureAtlas.format = VK_FORMAT_R8G8B8A8_SRGB;

    VkImageUsageFlags atlasImageUsages =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo atlasImageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .imageType = VK_IMAGE_TYPE_2D,
                                     .format = _textureAtlas.format,
                                     .extent = _textureAtlas.extent,
                                     .mipLevels = 1,
                                     .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = atlasImageUsages};

    VmaAllocationCreateInfo atlasAllocInfo{
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};

    VkResult ret = vmaCreateImage(_device.getAllocator(), &atlasImageInfo, &atlasAllocInfo,
                                  &_textureAtlas.image, &_textureAtlas.allocation, nullptr);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture atlas image");
    }

    // 6. Copy from staging buffer to the GPU image
    _commandExecutor->immediateSubmit([&](VkCommandBuffer cmd) {
        // Transition atlas to TRANSFER_DST_OPTIMAL
        _commandExecutor->transitionImage(cmd, _textureAtlas.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Copy buffer to image
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = _textureAtlas.extent;

        vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, _textureAtlas.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition atlas to SHADER_READ_ONLY_OPTIMAL
        _commandExecutor->transitionImage(cmd, _textureAtlas.image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    // 7. Clean up staging buffer and loaded images
    _bufferManager->destroyBuffer(stagingBuffer);
    for (auto& img : loadedImages) {
        stbi_image_free(img.pixels);
    }

    // 8. Create the Image View
    VkImageViewCreateInfo atlasViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = _textureAtlas.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = _textureAtlas.format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1}};

    ret = vkCreateImageView(_device.getDevice(), &atlasViewInfo, nullptr, &_textureAtlas.imageView);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture atlas image view");
    }

    // 9. Create the Sampler
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .magFilter = VK_FILTER_NEAREST, // Pixelated look for voxels
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .mipLodBias = 0.0F,
                                    .anisotropyEnable = VK_FALSE,
                                    .maxAnisotropy = 1.0F,
                                    .compareEnable = VK_FALSE,
                                    .compareOp = VK_COMPARE_OP_ALWAYS,
                                    .minLod = 0.0F,
                                    .maxLod = 0.0F,
                                    .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                    .unnormalizedCoordinates = VK_FALSE};

    ret = vkCreateSampler(_device.getDevice(), &samplerInfo, nullptr, &_textureAtlasSampler);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture atlas sampler");
    }

    std::cout << "Texture atlas loaded successfully!\n";
}

void Renderer::loadBlueNoiseTexture() {
    std::cout << "Loading blue noise texture...\n";

    // 1. Load pixels from disk
    int width, height, channels;
    stbi_uc* pixels = stbi_load("../../assets/textures/blue_noise/LDR_LLL1_0.png", &width, &height,
                                &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load blue noise texture");
    }

    // 2. Create GPU image (1 mip, GPU-only)
    _blueNoiseTexture.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    _blueNoiseTexture.format = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = _blueNoiseTexture.format,
                                .extent = _blueNoiseTexture.extent,
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage =
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

    VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY,
                                      .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};

    VkResult ret = vmaCreateImage(_device.getAllocator(), &imageInfo, &allocInfo,
                                  &_blueNoiseTexture.image, &_blueNoiseTexture.allocation, nullptr);
    if (ret != VK_SUCCESS) {
        stbi_image_free(pixels);
        throw std::runtime_error("Failed to create blue noise image");
    }

    // 3. Stage & upload
    AllocatedBuffer stagingBuffer =
        _bufferManager->createStagingBuffer(static_cast<VkDeviceSize>(width) * height * 4);

    memcpy(stagingBuffer.info.pMappedData, pixels, static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);

    _commandExecutor->immediateSubmit([&](VkCommandBuffer cmd) {
        // Transition to TRANSFER_DST_OPTIMAL
        _commandExecutor->transitionImage(cmd, _blueNoiseTexture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Copy buffer to image
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = _blueNoiseTexture.extent;

        vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, _blueNoiseTexture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition to SHADER_READ_ONLY_OPTIMAL
        _commandExecutor->transitionImage(cmd, _blueNoiseTexture.image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    _bufferManager->destroyBuffer(stagingBuffer);

    // 4. Create view
    VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = _blueNoiseTexture.image,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = _blueNoiseTexture.format,
                                   .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .baseMipLevel = 0,
                                                        .levelCount = 1,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1}};
    ret = vkCreateImageView(_device.getDevice(), &viewInfo, nullptr, &_blueNoiseTexture.imageView);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create blue noise image view");
    }

    // 5. Create sampler (nearest filter, repeat)
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_NEAREST,
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT};
    ret = vkCreateSampler(_device.getDevice(), &samplerInfo, nullptr, &_blueNoiseSampler);
    if (ret != VK_SUCCESS) {
        throw std::runtime_error("Failed to create blue noise sampler");
    }

    // 6. Register clean-up
    _mainDeletionQueue.push([this]() {
        vkDestroySampler(_device.getDevice(), _blueNoiseSampler, nullptr);
        vkDestroyImageView(_device.getDevice(), _blueNoiseTexture.imageView, nullptr);
        vmaDestroyImage(_device.getAllocator(), _blueNoiseTexture.image,
                        _blueNoiseTexture.allocation);
    });

    std::cout << "Blue noise texture loaded successfully!\n";
}

void Renderer::initSkyPipeline() {
    std::cout << "Initializing sky rendering pipeline...\n";

    // Load sky shaders
    std::ifstream vertFile("shaders/sky.vert.spv", std::ios::ate | std::ios::binary);
    std::ifstream fragFile("shaders/sky.frag.spv", std::ios::ate | std::ios::binary);

    if (!vertFile.is_open() || !fragFile.is_open()) {
        throw std::runtime_error("Failed to open sky shader files");
    }

    size_t vertSize = static_cast<size_t>(vertFile.tellg());
    size_t fragSize = static_cast<size_t>(fragFile.tellg());

    std::vector<char> vertCode(vertSize);
    std::vector<char> fragCode(fragSize);

    vertFile.seekg(0);
    fragFile.seekg(0);
    vertFile.read(vertCode.data(), static_cast<std::streamsize>(vertSize));
    fragFile.read(fragCode.data(), static_cast<std::streamsize>(fragSize));

    vertFile.close();
    fragFile.close();

    // Create shader modules
    VkShaderModuleCreateInfo vertModuleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0,
                                            .codeSize = vertCode.size(),
                                            .pCode =
                                                reinterpret_cast<const uint32_t*>(vertCode.data())};

    VkShaderModuleCreateInfo fragModuleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0,
                                            .codeSize = fragCode.size(),
                                            .pCode =
                                                reinterpret_cast<const uint32_t*>(fragCode.data())};

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;

    VkResult ret = vkCreateShaderModule(_device.getDevice(), &vertModuleInfo, nullptr, &vertModule);
    checkVkResult(ret, "Failed to create sky vertex shader module");

    ret = vkCreateShaderModule(_device.getDevice(), &fragModuleInfo, nullptr, &fragModule);
    checkVkResult(ret, "Failed to create sky fragment shader module");

    // Create descriptor set layout for blue noise texture
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    _skyDescriptorSetLayout =
        layoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_FRAGMENT_BIT);

    // Push constant range for sky rendering
    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(glm::mat4) + sizeof(float) * 4 // mat4 + timeOfDay + 3 padding floats
    };

    // Create pipeline layout with descriptor set
    VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &_skyDescriptorSetLayout,
                                          .pushConstantRangeCount = 1,
                                          .pPushConstantRanges = &pushConstantRange};

    ret = vkCreatePipelineLayout(_device.getDevice(), &layoutInfo, nullptr, &_skyPipelineLayout);
    checkVkResult(ret, "Failed to create sky pipeline layout");

    // Shader stages
    VkPipelineShaderStageCreateInfo vertStageInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertModule,
        .pName = "main",
        .pSpecializationInfo = nullptr};

    VkPipelineShaderStageCreateInfo fragStageInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragModule,
        .pName = "main",
        .pSpecializationInfo = nullptr};

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStageInfo, fragStageInfo};

    // Vertex input state - no vertex buffers needed (full-screen triangle)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr};

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE};

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, // Dynamic
        .scissorCount = 1,
        .pScissors = nullptr // Dynamic
    };

    // Rasterization state - no culling for sky
    VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, // No culling
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0F,
        .depthBiasClamp = 0.0F,
        .depthBiasSlopeFactor = 0.0F,
        .lineWidth = 1.0F};

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0F,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE};

    // Depth stencil - render at max depth, no depth writes
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_FALSE,                  // Don't write to depth buffer
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL, // Draw behind everything
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0F,
        .maxDepthBounds = 1.0F};

    // Color blend
    VkPipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};

    VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
        .blendConstants = {0.0F, 0.0F, 0.0F, 0.0F}};

    // Dynamic state
    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data()};

    // Rendering info for dynamic rendering
    VkFormat colorFormat = _renderContext->getDrawImage().format;
    VkFormat depthFormat = _renderContext->getDepthImage().format;

    VkPipelineRenderingCreateInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = depthFormat,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .flags = 0,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = _skyPipelineLayout,
        .renderPass = VK_NULL_HANDLE, // Using dynamic rendering
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};

    ret = vkCreateGraphicsPipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                    &_skyPipeline);
    checkVkResult(ret, "Failed to create sky graphics pipeline");

    // Allocate and write descriptor set for blue noise texture
    _skyDescriptorSet =
        _globalDescriptorAllocator.allocate(_device.getDevice(), _skyDescriptorSetLayout);
    DescriptorWriter writer;
    writer.writeImage(0, _blueNoiseTexture.imageView, _blueNoiseSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.updateSet(_device.getDevice(), _skyDescriptorSet);

    // Clean up shader modules
    vkDestroyShaderModule(_device.getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(_device.getDevice(), fragModule, nullptr);

    // Register cleanup
    _mainDeletionQueue.push([this]() {
        vkDestroyPipeline(_device.getDevice(), _skyPipeline, nullptr);
        vkDestroyPipelineLayout(_device.getDevice(), _skyPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device.getDevice(), _skyDescriptorSetLayout, nullptr);
    });

    std::cout << "Sky pipeline initialized successfully!\n";
}

void Renderer::drawSky(VkCommandBuffer cmd, float timeOfDay) {
    // Get draw and depth images
    const RenderContext::AllocatedImage& drawImage = _renderContext->getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _renderContext->getDepthImage();

    // Begin rendering to draw image
    VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = drawImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, // Clear for sky
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {0.0F, 0.0F, 0.0F, 1.0F}}};

    VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depthImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, // Clear depth buffer
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {1.0F, 0}}};

    VkRenderingInfo renderInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {.offset = {0, 0},
                       .extent = {drawImage.extent.width, drawImage.extent.height}},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
        .pStencilAttachment = nullptr};

    vkCmdBeginRendering(cmd, &renderInfo);

    // Bind sky pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipeline);

    // Bind descriptor set for blue noise texture
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipelineLayout, 0, 1,
                            &_skyDescriptorSet, 0, nullptr);

    // Set viewport and scissor
    VkViewport viewport{.x = 0.0F,
                        .y = 0.0F,
                        .width = static_cast<float>(drawImage.extent.width),
                        .height = static_cast<float>(drawImage.extent.height),
                        .minDepth = 0.0F,
                        .maxDepth = 1.0F};

    VkRect2D scissor{.offset = {0, 0}, .extent = {drawImage.extent.width, drawImage.extent.height}};

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Prepare push constants
    // Calculate view matrix without translation (for skybox effect)
    glm::mat4 view = _camera->getViewMatrix();
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view)); // Remove translation component

    // Calculate projection matrix (80 degree FOV as per requirements)
    float aspect =
        static_cast<float>(drawImage.extent.width) / static_cast<float>(drawImage.extent.height);
    glm::mat4 projection = glm::perspective(glm::radians(80.0F), aspect, 0.1F, 1000.0F);

    // Vulkan uses inverted Y axis
    projection[1][1] *= -1.0F;

    // Calculate inverse of viewProjection
    glm::mat4 viewProjection = projection * viewNoTranslation;
    glm::mat4 inverseViewProj = glm::inverse(viewProjection);

    // Push constants struct (must match shader layout)
    struct SkyPushConstants {
        glm::mat4 inverseViewProj;
        float timeOfDay;
        float padding1;
        float padding2;
        float padding3;
    } pushConstants;

    pushConstants.inverseViewProj = inverseViewProj;
    pushConstants.timeOfDay = timeOfDay;
    pushConstants.padding1 = 0.0F;
    pushConstants.padding2 = 0.0F;
    pushConstants.padding3 = 0.0F;

    vkCmdPushConstants(cmd, _skyPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(SkyPushConstants), &pushConstants);

    // Draw full-screen triangle (3 vertices, no vertex buffer needed)
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

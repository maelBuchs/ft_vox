#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vk_mem_alloc.h>

#include <SDL3/SDL_events.h>
#include <vulkan/vulkan.h>

#include "common/Types/RenderTypes.hpp"
#include "Core/DeletionQueue.hpp"
#include "Core/VulkanTypes.hpp"
#include "Memory/DescriptorAllocator.hpp"
#include "Pipeline/Pipeline.hpp"
#include "Rendering/RenderContext.hpp"

class VulkanDevice;
class VulkanSwapchain;
class Window;
class VulkanBuffer;
class MeshManager;
class Chunk;
class BlockRegistry;
class Camera;
class FrameManager;
class RenderContext;
class CommandExecutor;
class VoxelRenderer;

class Renderer {
  public:
    Renderer(Window& window, VulkanDevice& device, BlockRegistry& registry);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    static constexpr uint64_t VULKAN_TIMEOUT_NS = 1000000000; // 1 second
    void draw(float timeOfDay);
    void resizeSwapchain();
    void updateFPS(float deltaTime);
    void createDrawImages(VkExtent2D extent);
    void destroyDrawImages();
    void setWireframeMode(bool enabled) { _wireframeMode = enabled; }
    [[nodiscard]] bool isWireframeMode() const { return _wireframeMode; }
    [[nodiscard]] float getFPS() const { return _fps; }
    [[nodiscard]] Camera& getCamera() { return *_camera; }
    [[nodiscard]] DescriptorAllocatorGrowable& getGlobalDescriptorAllocator() {
        return _globalDescriptorAllocator;
    }

    [[nodiscard]] uint32_t getTextureId(const std::string& path);

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void initImGui();
    void loadTextureAtlas();
    void initSkyPipeline();
    void drawSky(VkCommandBuffer cmd, float timeOfDay);

    Window& _window;
    VulkanDevice& _device;
    BlockRegistry& _blockRegistry;
    std::unique_ptr<VulkanSwapchain> _swapchain;
    DescriptorAllocatorGrowable _globalDescriptorAllocator;
    std::vector<VkSemaphore> _swapchainSemaphores;
    std::vector<VkSemaphore> _renderSemaphores;
    DeletionQueue _mainDeletionQueue;
    std::unique_ptr<VulkanBuffer> _bufferManager;
    std::unique_ptr<MeshManager> _meshManager;
    std::unique_ptr<Camera> _camera;
    std::unique_ptr<FrameManager> _frameManager;
    std::unique_ptr<RenderContext> _renderContext;
    std::unique_ptr<CommandExecutor> _commandExecutor;
    std::unique_ptr<VoxelRenderer> _voxelRenderer;

    // Sky rendering
    VkPipeline _skyPipeline = VK_NULL_HANDLE;
    VkPipelineLayout _skyPipelineLayout = VK_NULL_HANDLE;

    // Wireframe mode
    bool _wireframeMode = false;

    // FPS tracking
    float _fps = 0.0f;
    float _frameTimeAccumulator = 0.0f;
    int _frameCount = 0;

    RenderContext::AllocatedImage _textureAtlas{};
    VkSampler _textureAtlasSampler = VK_NULL_HANDLE;
    std::unordered_map<std::string, uint32_t> _texturePathToId;
    uint32_t _nextTextureId = 0;
    int _atlasTexturesPerRow = 0; // Number of textures per row in atlas
};

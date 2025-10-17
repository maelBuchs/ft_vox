#pragma once

#include <array>
#include <memory>
#include <vk_mem_alloc.h>

#include <SDL3/SDL_events.h>
#include <vulkan/vulkan.h>

#include "common/Types/RenderTypes.hpp"
#include "DeletionQueue.hpp"
#include "DescriptorAllocator.hpp"
#include "Pipeline.hpp"
#include "VulkanTypes.hpp"

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
    void draw();
    void resizeSwapchain();
    void updateFPS(float deltaTime);
    void createDrawImages(VkExtent2D extent);
    void destroyDrawImages();
    void setWireframeMode(bool enabled) { _wireframeMode = enabled; }
    [[nodiscard]] bool isWireframeMode() const { return _wireframeMode; }
    [[nodiscard]] float getFPS() const { return _fps; }
    [[nodiscard]] Camera& getCamera() { return *_camera; }

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void initImGui();

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

    // Wireframe mode
    bool _wireframeMode = false;

    // FPS tracking
    float _fps = 0.0f;
    float _frameTimeAccumulator = 0.0f;
    int _frameCount = 0;
};

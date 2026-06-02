#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include <SDL3/SDL_events.h>
#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "common/Protocol/Protocol.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "Core/DeletionQueue.hpp"
#include "Memory/DescriptorAllocator.hpp"
#include "Pipeline/Pipeline.hpp"
#include "Rendering/RenderContext.hpp"
#include "server/World/WorldManager.hpp"

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
    Renderer(Window& window, VulkanDevice& device, BlockRegistry& blockRegistry,
             std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues,
             WorldManager* worldManager);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void updateMeshes(const glm::ivec3& cameraChunkPos,
                      int maxLoadDistance); // Process finished mesh data from worker threads
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
    [[nodiscard]] uint64_t getFrameNumber() const;

    [[nodiscard]] uint32_t getTextureId(const std::string& path);

    // Chunk unloading support
    void rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks);
    [[nodiscard]] size_t getLoadedChunkCount() const;
    [[nodiscard]] float getMeshPoolUsage() const;
    [[nodiscard]] uint32_t getLodResidentCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodSubmittedCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodMeshUpdateCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodUpgradeCount() const;
    [[nodiscard]] uint32_t getLodDowngradeCount() const;

    [[nodiscard]] bool needsGammaCorrection() const;

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void initImGui();
    void loadTextureAtlas();
    void loadBlueNoiseTexture();
    void initSkyPipeline();
    void drawSky(VkCommandBuffer cmd, float timeOfDay);

    void initOutlinePipeline();
    void drawOutline(VkCommandBuffer cmd, const glm::vec3& targetBlockPos);

    Window& _window;
    VulkanDevice& _device;
    BlockRegistry& _blockRegistry;
    std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>&
        _perThreadMeshQueues; // Reference to mesh queue from App
    std::unique_ptr<VulkanSwapchain> _swapchain;
    DescriptorAllocatorGrowable _globalDescriptorAllocator;
    std::vector<VkSemaphore> _swapchainSemaphores;
    std::vector<VkSemaphore> _renderSemaphores;
    std::vector<VkFence> _imagesInFlight;
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
    VkDescriptorSetLayout _skyDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _skyDescriptorSet = VK_NULL_HANDLE;

    // Outline rendering
    VkPipeline _outlinePipeline = VK_NULL_HANDLE;
    VkPipelineLayout _outlinePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _outlineDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _outlineDescriptorSet = VK_NULL_HANDLE;

    // Wireframe mode
    bool _wireframeMode = false;

    // FPS tracking
    float _fps = 0.0F;
    float _frameTimeAccumulator = 0.0F;
    int _frameCount = 0;

    RenderContext::AllocatedImage _textureAtlas{};
    VkSampler _textureAtlasSampler = VK_NULL_HANDLE;
    std::unordered_map<std::string, uint32_t> _texturePathToId;
    uint32_t _nextTextureId = 0;
    int _atlasTexturesPerRow = 0; // Number of textures per row in atlas

    // Blue noise texture for dithering
    RenderContext::AllocatedImage _blueNoiseTexture{};
    VkSampler _blueNoiseSampler = VK_NULL_HANDLE;

    // ImGui
    VkDescriptorPool _imguiPool = VK_NULL_HANDLE;

    WorldManager* _worldManager = nullptr;
};

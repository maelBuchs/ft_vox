#pragma once

#include <array>
#include <memory>
#include <vk_mem_alloc.h>

#include <SDL3/SDL_events.h>
#include <vulkan/vulkan.h>

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

class Renderer {
  public:
    Renderer(Window& window, VulkanDevice& device, BlockRegistry& registry);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    struct FrameData {
        VkCommandPool _commandPool{};
        VkCommandBuffer _mainCommandBuffer{};
        VkFence _renderFence{};
        VkSemaphore _swapchainSemaphore{};
        VkSemaphore _renderSemaphore{};
        DeletionQueue _deletionQueue;
        DescriptorAllocatorGrowable _frameDescriptors;
    };

    struct AllocatedImage {
        VkImage image;
        VkImageView imageView;
        VmaAllocation allocation;
        VkExtent3D extent;
        VkFormat format;
    };

    static constexpr unsigned int FRAME_OVERLAP = 2;
    static constexpr uint64_t VULKAN_TIMEOUT_NS = 1000000000; // 1 second

    FrameData& getCurrentFrame() { return _frameData.at(_frameNumber % FRAME_OVERLAP); }
    void draw();
    void resizeSwapchain();
    void processInput(SDL_Event& event);
    void updateCamera(float deltaTime);
    void updateFPS(float deltaTime);
    void createDrawImages(VkExtent2D extent);
    void destroyDrawImages();
    void setWireframeMode(bool enabled) { _wireframeMode = enabled; }
    [[nodiscard]] bool isWireframeMode() const { return _wireframeMode; }
    [[nodiscard]] float getFPS() const { return _fps; }

  private:
    static void checkVkResult(VkResult result, const char* errorMessage);
    void createFrameCommandPools();
    void createFrameSyncStructures();
    VkImageMemoryBarrier2 createImageBarrier(VkImage image, VkImageLayout oldLayout,
                                             VkImageLayout newLayout) const;
    static VkImageAspectFlags getImageAspectMask(VkImageLayout layout);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout) const;
    void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination,
                             VkExtent2D srcSize, VkExtent2D dstSize);
    void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
    void initImGui();
    void initVoxelPipeline();
    void initTestChunk();
    void drawVoxels(VkCommandBuffer cmd);

    Window& _window;
    VulkanDevice& _device;
    BlockRegistry& _blockRegistry;
    std::unique_ptr<VulkanSwapchain> _swapchain;
    DescriptorAllocatorGrowable _globalDescriptorAllocator;
    uint64_t _frameNumber;
    std::array<FrameData, FRAME_OVERLAP> _frameData;
    AllocatedImage _drawImage;
    AllocatedImage _depthImage;
    VkExtent2D _drawExtent;
    DeletionQueue _mainDeletionQueue;
    VkFence _immFence;
    VkCommandPool _immCommandPool;
    VkCommandBuffer _immCommandBuffer;
    std::unique_ptr<VulkanBuffer> _bufferManager;
    std::unique_ptr<MeshManager> _meshManager;
    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;
    GPUMeshBuffers _testChunkMesh;
    std::unique_ptr<Chunk> _testChunk;
    glm::vec3 _cameraPos;
    glm::vec3 _cameraFront;
    glm::vec3 _cameraUp;

    // Camera control
    float _cameraSpeed = 0.1f;
    float _cameraSensitivity = 0.050f;
    float _cameraYaw = -90.0f;
    float _cameraPitch = 0.0f;

    // Wireframe mode
    bool _wireframeMode = false;

    // FPS tracking
    float _fps = 0.0f;
    float _frameTimeAccumulator = 0.0f;
    int _frameCount = 0;
};

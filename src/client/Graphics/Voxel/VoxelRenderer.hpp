#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "../Core/VulkanTypes.hpp"
#include "../Pipeline/Pipeline.hpp"
#include "common/Protocol/Protocol.hpp"
#include "common/Types/RenderTypes.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "MeshBufferPool.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

class VulkanDevice;
class MeshManager;
class BlockRegistry;
class Camera;
class RenderContext;
class CommandExecutor;
class MeshBufferPool;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
class Renderer;

class VoxelRenderer {
  public:
    VoxelRenderer(VulkanDevice& device, MeshManager& meshManager, BlockRegistry& registry,
                  RenderContext& context, CommandExecutor& executor, VulkanBuffer& bufferManager,
                  DescriptorAllocatorGrowable& descriptorAllocator, Renderer& renderer,
                  ThreadSafeQueue<MeshData>& finishedMeshQueue);
    ~VoxelRenderer();

    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;
    VoxelRenderer(VoxelRenderer&&) = delete;
    VoxelRenderer& operator=(VoxelRenderer&&) = delete;

    void initPipelines(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow);

    /**
     * Update: Check for finished meshes and upload to GPU.
     * Call this every frame BEFORE drawVoxels.
     * @param cameraChunkPos Current camera chunk position for distance checks
     * @param maxLoadDistance Maximum distance (in chunks) to accept new meshes
     */
    void update(const glm::ivec3& cameraChunkPos, int maxLoadDistance);

    void drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode);

    /**
     * Rebuild the entire mesh pool and clear all chunk meshes.
     * Call this when chunks are unloaded to free VRAM.
     * WARNING: This waits for GPU idle - may cause a frame hitch.
     *
     * @param unloadedChunks List of chunk positions that were unloaded from RAM
     */
    void rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks);

    /**
     * Get the current number of chunks loaded in the renderer.
     */
    [[nodiscard]] size_t getLoadedChunkCount() const { return _chunkDrawInfos.size(); }

    /**
     * Get mesh buffer pool usage (0.0 to 1.0).
     * Returns the maximum of vertex and index buffer usage.
     */
    [[nodiscard]] float getMeshPoolUsage() const;

  private:
    void initMDI(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow);
    void initComputeCulling();
    void ensureBufferCapacity(uint32_t requiredChunks);

    static constexpr uint32_t MAX_CHUNKS =
        16384; // Initial maximum number of chunks that can be rendered
    static constexpr float TYPICAL_MAX_VRAM_MB = 512.0f; // Assume 512MB as "100%" for UI display

    VulkanDevice& _device;
    MeshManager& _meshManager;
    BlockRegistry& _blockRegistry;
    RenderContext& _context;
    CommandExecutor& _executor;
    VulkanBuffer& _bufferManager;
    DescriptorAllocatorGrowable& _descriptorAllocator;
    Renderer& _renderer;

    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;

    VkPipelineLayout _voxelPipelineLayout = VK_NULL_HANDLE;

    // --- MDI Resources ---
    std::unique_ptr<MeshBufferPool> _meshPool;

    struct ChunkDrawInfo {
        glm::ivec3 chunkCoords{};
        glm::vec3 worldPosition{};
        ChunkMeshBuffers meshBuffers{}; // Per-chunk VMA-allocated buffers
    };

    std::vector<ChunkDrawInfo> _chunkDrawInfos;
    std::unordered_map<glm::ivec3, size_t> _chunkDrawLookup;

    // Queue for receiving finished mesh data from meshing threads
    ThreadSafeQueue<MeshData>& _finishedMeshQueue;

    AllocatedBuffer _indirectBuffer;
    AllocatedBuffer _chunkDataBuffer;
    AllocatedBuffer _atlasConfigBuffer;      // Uniform buffer for atlas configuration
    uint32_t _currentMaxChunks = MAX_CHUNKS; // Current capacity of indirect/chunk buffers

    std::vector<VkDrawIndexedIndirectCommand> _indirectCommands;
    std::vector<GPUChunkData> _chunkDrawData;

    // Descriptor set for chunk data SSBO
    VkDescriptorSetLayout _chunkSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _chunkDescriptorSet = VK_NULL_HANDLE; // Points to _chunkDataBuffer (input)
    VkDescriptorSet _culledChunkDescriptorSet =
        VK_NULL_HANDLE; // Points to _culledChunkDataBuffer (output)

    // --- GPU Frustum Culling Resources ---
    Pipeline _frustumCullPipeline;
    VkPipelineLayout _frustumCullPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _frustumCullSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _frustumCullDescriptorSet = VK_NULL_HANDLE;

    // Buffers for compute culling
    AllocatedBuffer _frustumUniformBuffer;  // GPUFrustumData uniform buffer
    AllocatedBuffer _culledIndirectBuffer;  // Output indirect commands from compute
    AllocatedBuffer _culledChunkDataBuffer; // Output chunk data from compute

    // Push constants for compute shader
    struct ComputePushConstants {
        uint32_t totalChunks;
        float chunkSize;
        uint32_t debugMode; // Bit flags: 1=skip distance, 2=skip sphere, 4=skip AABB
        uint32_t _padding2;
    };

    // Statistics
    CullingStats _cullingStats{};

    // GPU frustum culling is currently DISABLED because overhead > savings
    // Current measurements: Culling takes 300μs/frame, but draw calls only take 13.8μs/frame
    // Re-enable when rendering 1000+ chunks, not ~200 chunks
    // Performance profile shows: indirect draw is extremely cheap on modern GPUs
    bool _enableGPUCulling = false;          // DISABLED - overhead too high for chunk count
    bool _supportsDrawIndirectCount = false; // Runtime feature check

    // Frustum caching (avoid rebuilding/uploading every frame)
    mutable glm::mat4 _cachedProjection{0.0f};
    mutable std::array<glm::vec4, 6> _cachedFrustumPlanes{};
    mutable glm::vec3 _cachedCameraPos{0.0f};
    mutable bool _frustumDirty = true;
};

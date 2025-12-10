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
                  std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues);
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
    void initMeshShaderPipeline(VkImageView atlasView, VkSampler atlasSampler);
    void ensureBufferCapacity(uint32_t requiredChunks);

    static constexpr uint32_t MAX_CHUNKS =
        2048; // Initial maximum number of chunks (grows dynamically via ensureBufferCapacity)
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
    Pipeline _meshShaderPipeline; // Task/mesh shader pipeline

    VkPipelineLayout _voxelPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout _meshShaderPipelineLayout = VK_NULL_HANDLE;

    // --- MDI Resources ---
    std::unique_ptr<MeshBufferPool> _meshPool;

    struct ChunkDrawInfo {
        glm::ivec3 chunkCoords{};
        glm::vec3 worldPosition{};
        ChunkMeshBuffers meshBuffers{}; // Per-chunk VMA-allocated buffers
    };

    std::vector<ChunkDrawInfo> _chunkDrawInfos;
    std::unordered_map<glm::ivec3, size_t> _chunkDrawLookup;

    // Per-thread mesh queues for receiving finished mesh data (eliminates lock contention)
    std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& _perThreadMeshQueues;

    AllocatedBuffer _indirectBuffer;
    static constexpr uint32_t CHUNK_BUFFER_COUNT = 2; // Must match FrameManager::FRAME_OVERLAP
    std::array<AllocatedBuffer, CHUNK_BUFFER_COUNT> _chunkDataBuffers;
    AllocatedBuffer _atlasConfigBuffer;      // Uniform buffer for atlas configuration
    uint32_t _currentMaxChunks = MAX_CHUNKS; // Current capacity of indirect/chunk buffers

    std::vector<VkDrawIndexedIndirectCommand> _indirectCommands;
    std::vector<GPUChunkData> _chunkDrawData;
    bool _chunkDataDirty = true; // Track if chunk data needs GPU upload

    // Max load distance for dynamic render distance culling
    int _maxLoadDistance = 24; // Default to 24 chunks

    // Upload throttling to reduce GPU overhead
    uint32_t _dirtyChunkCount = 0;
    std::chrono::steady_clock::time_point _lastUploadTime = std::chrono::steady_clock::now();
    static constexpr uint32_t MIN_CHUNKS_FOR_UPLOAD = 32;      // Batch at least this many
    static constexpr uint32_t MAX_MS_BETWEEN_UPLOADS = 16;     // Or upload every 16ms
    static constexpr uint32_t MAX_MESHES_PER_BATCH = 256;      // Process max 256 meshes/frame

    uint32_t _adaptiveMinChunks = MIN_CHUNKS_FOR_UPLOAD;       // Adjusts between 8-64
    uint32_t _adaptiveMaxMs = MAX_MS_BETWEEN_UPLOADS;          // Adjusts between 8-32ms
    uint32_t _adaptiveMaxMeshes = MAX_MESHES_PER_BATCH;        // Adjusts between 64-512
    std::chrono::steady_clock::time_point _lastFrameTime = std::chrono::steady_clock::now();
    float _avgFrameTimeMs = 16.67f; // Target 60 FPS

    std::vector<bool> _dirtyChunkIndices; // Tracks which specific chunks need upload
    std::vector<uint32_t> _dirtyChunkList; // Compact list of dirty indices (for efficient iteration)

    // Descriptor set for chunk data SSBO
    VkDescriptorSetLayout _chunkSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, CHUNK_BUFFER_COUNT> _chunkDescriptorSets = {}; // Points to _chunkDataBuffers[i] (input)
    VkDescriptorSet _culledChunkDescriptorSet =
        VK_NULL_HANDLE; // Points to _culledChunkDataBuffer (output)

    // Traditional path fragment descriptor (Set 1: texture atlas)
    VkDescriptorSetLayout _traditionalFragSetLayout = VK_NULL_HANDLE; // Set 1: Texture atlas for fragment
    VkDescriptorSet _traditionalFragDescriptorSet = VK_NULL_HANDLE;   // Set 1 descriptor set

    // Mesh shader pipeline resources
    VkDescriptorSetLayout _meshShaderSetLayout =
        VK_NULL_HANDLE; // Set 0: Camera, chunk data, index buffer
    VkDescriptorSetLayout _meshShaderFragSetLayout =
        VK_NULL_HANDLE;                                        // Set 1: Texture atlas for fragment
    std::array<VkDescriptorSet, CHUNK_BUFFER_COUNT> _meshShaderDescriptorSets = {}; // Set 0 descriptor sets
    VkDescriptorSet _meshShaderFragDescriptorSet = VK_NULL_HANDLE; // Set 1 descriptor set
    Pipeline _meshShaderWireframePipeline; // Wireframe variant for debugging (F1 toggle)
    AllocatedBuffer _cameraUniformBuffer;  // GPUCameraData UBO for task/mesh shaders
    bool _useMeshShaders = false;          // Runtime toggle for mesh shader path
    PFN_vkCmdDrawMeshTasksEXT _vkCmdDrawMeshTasksEXT = nullptr; // Extension function pointer
    uint32_t _maxMeshWorkgroupsPerTask = 1; // Device limit for mesh workgroups per task workgroup

    // Flag to defer index buffer resize descriptor updates to safe frame point
    bool _indexBufferResizePending = false;

    // --- GPU Frustum Culling Resources (DISABLED - incompatible with mesh shaders) ---
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

    // GPU frustum culling is DISABLED - task shader does culling instead
    // Compute shader path is fundamentally incompatible with mesh shader pipeline
    // Task shader performs per-chunk culling before mesh shader dispatch
    // For traditional path: overhead > savings (300μs culling vs 13.8μs draw)
    bool _enableGPUCulling = false;          // DISABLED - mesh shaders use task shader culling
    bool _supportsDrawIndirectCount = false; // Runtime feature check
};

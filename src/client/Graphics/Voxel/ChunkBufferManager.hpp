#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "../Core/VulkanTypes.hpp"
#include "common/Protocol/Protocol.hpp"
#include "common/Types/RenderTypes.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "MeshBufferPool.hpp"

class VulkanDevice;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
class CommandExecutor;
class MeshBufferPool;
class Renderer;
class VoxelPipelineManager;

/**
 * @brief Manages chunk GPU buffers, descriptor sets, and data upload.
 *
 * Handles:
 * - Per-chunk mesh buffer allocation/deallocation via MeshBufferPool
 * - GPU buffer management (indirect, chunk data, frustum, camera)
 * - Descriptor set allocation and updates
 * - Dirty chunk tracking and throttled GPU upload
 */
class ChunkBufferManager {
  public:
    static constexpr uint32_t CHUNK_BUFFER_COUNT = 2;
    static constexpr uint32_t MAX_CHUNKS = 2048;

    struct ChunkDrawInfo {
        glm::ivec3 chunkCoords{};
        glm::vec3 worldPosition{};
        ChunkMeshBuffers meshBuffers{};
    };

    ChunkBufferManager(VulkanDevice& device, VulkanBuffer& bufferManager,
                       DescriptorAllocatorGrowable& descriptorAllocator,
                       CommandExecutor& executor, Renderer& renderer,
                       VoxelPipelineManager& pipelineManager,
                       std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues);
    ~ChunkBufferManager();

    ChunkBufferManager(const ChunkBufferManager&) = delete;
    ChunkBufferManager& operator=(const ChunkBufferManager&) = delete;
    ChunkBufferManager(ChunkBufferManager&&) = delete;
    ChunkBufferManager& operator=(ChunkBufferManager&&) = delete;

    // --- Initialization ---

    /**
     * @brief Initialize all GPU buffers and descriptor sets.
     * @param atlasView Texture atlas image view
     * @param atlasSampler Texture atlas sampler
     * @param texturesPerRow Textures per row in atlas (for config buffer)
     */
    void init(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow);

    // --- Frame Updates ---

    /**
     * @brief Process mesh queues and update GPU buffers.
     * @param cameraChunkPos Current camera chunk position for distance checks
     * @param maxLoadDistance Maximum distance (in chunks) to accept new meshes
     */
    void update(const glm::ivec3& cameraChunkPos, int maxLoadDistance);

    /**
     * @brief Process deletion queue and handle index buffer resize.
     * Must be called at the start of each frame before drawing.
     */
    void beginFrame();

    /**
     * @brief Rebuild mesh pool for unloaded chunks.
     * @param unloadedChunks List of chunk positions that were unloaded
     */
    void rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks);

    // --- Accessors ---

    [[nodiscard]] const std::vector<ChunkDrawInfo>& getChunkDrawInfos() const { return _chunkDrawInfos; }
    [[nodiscard]] const std::vector<GPUChunkData>& getChunkDrawData() const { return _chunkDrawData; }
    [[nodiscard]] size_t getLoadedChunkCount() const { return _chunkDrawInfos.size(); }
    [[nodiscard]] float getMeshPoolUsage() const;
    [[nodiscard]] int getMaxLoadDistance() const { return _maxLoadDistance; }

    // Buffers
    [[nodiscard]] VkBuffer getIndirectBuffer() const { return _indirectBuffer.buffer; }
    [[nodiscard]] AllocatedBuffer& getIndirectBufferAllocation() { return _indirectBuffer; }
    [[nodiscard]] const AllocatedBuffer& getIndirectBufferAllocation() const { return _indirectBuffer; }
    [[nodiscard]] AllocatedBuffer& getChunkDataBuffer(uint32_t frameIndex) { return _chunkDataBuffers[frameIndex]; }
    [[nodiscard]] AllocatedBuffer& getFrustumUniformBuffer() { return _frustumUniformBuffer; }
    [[nodiscard]] AllocatedBuffer& getCulledIndirectBuffer() { return _culledIndirectBuffer; }
    [[nodiscard]] AllocatedBuffer& getCulledChunkDataBuffer() { return _culledChunkDataBuffer; }
    [[nodiscard]] AllocatedBuffer& getCameraUniformBuffer(uint32_t frameIndex) { return _cameraUniformBuffers[frameIndex]; }

    // Descriptor sets
    [[nodiscard]] VkDescriptorSet getChunkDescriptorSet(uint32_t frameIndex) const { return _chunkDescriptorSets[frameIndex]; }
    [[nodiscard]] VkDescriptorSet getCulledChunkDescriptorSet() const { return _culledChunkDescriptorSet; }
    [[nodiscard]] VkDescriptorSet getTraditionalFragDescriptorSet() const { return _traditionalFragDescriptorSet; }
    [[nodiscard]] VkDescriptorSet getFrustumCullDescriptorSet() const { return _frustumCullDescriptorSet; }
    [[nodiscard]] VkDescriptorSet getMeshShaderDescriptorSet(uint32_t frameIndex) const { return _meshShaderDescriptorSets[frameIndex]; }
    [[nodiscard]] VkDescriptorSet getMeshShaderFragDescriptorSet() const { return _meshShaderFragDescriptorSet; }

    // Mesh pool access
    [[nodiscard]] MeshBufferPool& getMeshPool() { return *_meshPool; }
    [[nodiscard]] VkBuffer getIndexBuffer() const { return _meshPool->getIndexBuffer(); }
    [[nodiscard]] VkDeviceSize getIndexBufferCapacity() const { return _meshPool->getIndexBufferCapacity(); }

    // Indirect commands
    [[nodiscard]] const std::vector<VkDrawIndexedIndirectCommand>& getIndirectCommands() const { return _indirectCommands; }

    /**
     * @brief Ensure buffer capacity for the required number of chunks.
     */
    void ensureBufferCapacity(uint32_t requiredChunks);

    /**
     * @brief Get the current max chunks capacity.
     */
    [[nodiscard]] uint32_t getCurrentMaxChunks() const { return _currentMaxChunks; }

    /**
     * @brief Set the index buffer resize pending flag.
     */
    void setIndexBufferResizePending(bool pending) { _indexBufferResizePending = pending; }

    /**
     * @brief Upload chunk data to GPU (for traditional path).
     */
    void uploadChunkData(uint32_t frameIndex);

    /**
     * @brief Build indirect commands from chunk data.
     */
    void buildIndirectCommands();

  private:
    void allocateDescriptorSets(VkImageView atlasView, VkSampler atlasSampler);
    void initMeshShaderDescriptors(VkImageView atlasView, VkSampler atlasSampler);
    void initComputeCullingBuffers();
    void executeBufferResize();

    VulkanDevice& _device;
    VulkanBuffer& _bufferManager;
    DescriptorAllocatorGrowable& _descriptorAllocator;
    CommandExecutor& _executor;
    Renderer& _renderer;
    VoxelPipelineManager& _pipelineManager;
    std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& _perThreadMeshQueues;

    // --- Mesh Buffer Pool ---
    std::unique_ptr<MeshBufferPool> _meshPool;

    // --- Chunk Data ---
    std::vector<ChunkDrawInfo> _chunkDrawInfos;
    std::unordered_map<glm::ivec3, size_t> _chunkDrawLookup;
    std::vector<GPUChunkData> _chunkDrawData;
    std::vector<VkDrawIndexedIndirectCommand> _indirectCommands;

    // --- GPU Buffers ---
    AllocatedBuffer _indirectBuffer;
    std::array<AllocatedBuffer, CHUNK_BUFFER_COUNT> _chunkDataBuffers;
    AllocatedBuffer _atlasConfigBuffer;
    std::array<AllocatedBuffer, CHUNK_BUFFER_COUNT> _cameraUniformBuffers;
    AllocatedBuffer _frustumUniformBuffer;
    AllocatedBuffer _culledIndirectBuffer;
    AllocatedBuffer _culledChunkDataBuffer;

    uint32_t _currentMaxChunks = MAX_CHUNKS;

    // --- Descriptor Sets ---
    std::array<VkDescriptorSet, CHUNK_BUFFER_COUNT> _chunkDescriptorSets{};
    VkDescriptorSet _culledChunkDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet _traditionalFragDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet _frustumCullDescriptorSet = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, CHUNK_BUFFER_COUNT> _meshShaderDescriptorSets{};
    VkDescriptorSet _meshShaderFragDescriptorSet = VK_NULL_HANDLE;

    // --- Dirty Tracking ---
    bool _chunkDataDirty = true;
    uint32_t _dirtyChunkCount = 0;
    std::vector<bool> _dirtyChunkIndices;
    std::vector<uint32_t> _dirtyChunkList;
    std::chrono::steady_clock::time_point _lastUploadTime = std::chrono::steady_clock::now();

    // --- Reusable Buffers (avoid per-frame allocations) ---
    std::vector<MeshData> _meshBatch;                    // Reused each frame
    std::vector<MeshData> _retryMeshBatch;               // Meshes that failed to upload
    std::vector<GPUChunkData> _dirtyDataBuffer;          // Reused for dirty uploads
    std::vector<VkBufferCopy> _copyRegions;              // Reused for batched copies

    // --- Pending Buffer Resize (avoid vkDeviceWaitIdle) ---
    bool _bufferResizePending = false;
    uint32_t _pendingNewCapacity = 0;

    // --- Throttling Parameters ---
    int _maxLoadDistance = 24;
    static constexpr uint32_t MIN_CHUNKS_FOR_UPLOAD = 32;
    static constexpr uint32_t MAX_MS_BETWEEN_UPLOADS = 16;
    static constexpr uint32_t MAX_MESHES_PER_BATCH = 256;

    uint32_t _adaptiveMinChunks = MIN_CHUNKS_FOR_UPLOAD;
    uint32_t _adaptiveMaxMs = MAX_MS_BETWEEN_UPLOADS;
    uint32_t _adaptiveMaxMeshes = MAX_MESHES_PER_BATCH;
    std::chrono::steady_clock::time_point _lastFrameTime = std::chrono::steady_clock::now();
    float _avgFrameTimeMs = 16.67f;

    bool _indexBufferResizePending = false;
};

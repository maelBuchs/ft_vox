#pragma once

#include <functional>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Core/VulkanTypes.hpp"
#include "../Rendering/FrameManager.hpp"

class VulkanDevice;
class VulkanBuffer;

// ============================================================================
// PER-CHUNK BUFFER ARCHITECTURE (VMA-based)
// ============================================================================
// Each chunk mesh gets its own dedicated VkBuffer pair (vertex + index).
// This eliminates fragmentation entirely and provides O(1) allocation/deallocation.
// VMA (Vulkan Memory Allocator) handles all memory management automatically.
// ============================================================================

// ============================================================================
// HYBRID APPROACH: Per-chunk vertices + Mega index buffer
// ============================================================================
// - Vertex buffers: Per-chunk (VMA allocated), accessed via buffer device address
// - Index buffer: Single mega buffer (traditional), shared by all chunks
// This gives us:
// - Memory savings on vertices (the big part, 80%+ of data)
// - Multi-draw indirect still works (needs bound index buffer)
// - Scalability (thousands of chunks)
// ============================================================================

// Represents a chunk's mesh allocation (hybrid approach)
struct ChunkMeshBuffers {
    AllocatedBuffer vertexBuffer;      // Per-chunk VMA allocation
    VkDeviceAddress vertexAddress = 0; // For shader buffer_reference access
    uint32_t vertexCount = 0;

    // Index data stored in mega buffer (not per-chunk)
    uint32_t firstIndex = 0; // Offset in mega index buffer
    uint32_t indexCount = 0;
};

// Manages per-chunk buffer allocation using VMA
// No more mega-buffers, no more fragmentation!
class MeshBufferPool {
  public:
    MeshBufferPool(VulkanDevice& device, VulkanBuffer& bufferManager);
    ~MeshBufferPool();

    MeshBufferPool(const MeshBufferPool&) = delete;
    MeshBufferPool& operator=(const MeshBufferPool&) = delete;
    MeshBufferPool(MeshBufferPool&&) = delete;
    MeshBufferPool& operator=(MeshBufferPool&&) = delete;

    // Allocate per-chunk vertex buffer + index sub-allocation
    ChunkMeshBuffers allocateChunkBuffers(
        std::span<uint32_t> indices, std::span<uint32_t> vertices,
        const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit);

    // Free a chunk's vertex buffer (deferred until safe)
    // Index space is marked as free for reuse
    void freeChunkBuffers(const ChunkMeshBuffers& buffers);

    // Process deletion queue - call once per frame after GPU work is submitted
    void processDeletionQueue();

    // Force immediate cleanup (for shutdown)
    void flushDeletionQueue();

    // Fast shutdown - destroy all vertex buffers without queuing (GPU already idle)
    void destroyAllChunkBuffers(const std::vector<ChunkMeshBuffers>& buffers);

    // Reset index buffer offset (call when all chunks are unloaded)
    void resetIndexOffset();

    // Get buffers for binding
    [[nodiscard]] VkBuffer getIndexBuffer() const { return _indexBuffer.buffer; }

    // Get statistics
    [[nodiscard]] size_t getAllocatedChunkCount() const { return _allocatedChunks; }
    [[nodiscard]] size_t getTotalVertexMemory() const { return _totalVertexMemory; }
    [[nodiscard]] size_t getTotalIndexMemory() const { return _totalIndexMemory; }
    [[nodiscard]] VkDeviceSize getIndexBufferCapacity() const { return _currentIndexBufferSize; }
    [[nodiscard]] float getMemoryUsage() const;

    // PHASE 6: Public staging buffer pool API for all buffer types
    enum class StagingType { Vertex, Index, Generic };
    AllocatedBuffer acquireStagingBuffer(VkDeviceSize requiredSize, StagingType type);

  private:
    VulkanDevice& _device;
    VulkanBuffer& _bufferManager;

    // Mega index buffer (shared by all chunks) - now dynamic!
    AllocatedBuffer _indexBuffer;
    uint32_t _indexOffset = 0;          // Current write position in index buffer
    VkDeviceSize _currentIndexBufferSize = 0; // Current capacity in bytes (grows dynamically)

    // Statistics tracking
    size_t _allocatedChunks = 0;
    size_t _totalVertexMemory = 0;
    size_t _totalIndexMemory = 0;

    // Debugging counters to track buffer lifecycle
    size_t _totalAllocations = 0;
    size_t _totalFrees = 0;

    // Deletion queue - vertex buffers are added here and deleted after 2 frames
    // This ensures the GPU is done using them
    struct DeletionEntry {
        AllocatedBuffer vertexBuffer;
        uint32_t vertexCount;
        uint32_t frameDelay = FrameManager::FRAME_OVERLAP; // Wait for frame overlap before deleting
    };
    std::vector<DeletionEntry> _deletionQueue;

    // ========================================================================
    // FREE-LIST INDEX ALLOCATOR
    // ========================================================================
    // Recycles freed index ranges to prevent monotonic growth and exhaustion
    struct IndexRange {
        uint32_t start; // Starting index in mega buffer
        uint32_t count; // Number of indices
    };

    struct PendingIndexFree {
        uint32_t start;
        uint32_t count;
        uint32_t frameDelay =
            FrameManager::FRAME_OVERLAP; // Wait for frame overlap before recycling
    };

    std::vector<IndexRange> _freeIndexRanges;         // Available ranges (sorted by start)
    std::vector<PendingIndexFree> _pendingIndexFrees; // Awaiting GPU completion

    // Helper methods for index allocation
    void returnIndexRangeToFreeList(uint32_t start, uint32_t count);
    void processPendingIndexFrees();
    void ensureIndexBufferCapacity(
        uint32_t requiredIndices,
        const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit);

    // ========================================================================
    // PHASE 3: BATCHED UPLOAD SYSTEM
    // ========================================================================
    // Instead of immediate submits per chunk, queue all uploads and submit once
    struct PendingUpload {
        AllocatedBuffer stagingVertex;
        AllocatedBuffer stagingIndex;
        VkBuffer dstVertexBuffer;
        VkBuffer dstIndexBuffer;
        VkDeviceSize vertexSize;
        VkDeviceSize indexSize;
        uint32_t indexOffset; // Offset in mega index buffer
    };
    std::vector<PendingUpload> _pendingUploads;

    // ========================================================================
    // PHASE 4: STAGING BUFFER POOL (eliminates create/destroy overhead)
    // ========================================================================
    struct StagingBuffer {
        AllocatedBuffer buffer;
        VkDeviceSize size;
        uint32_t frameDelay = 0; // 0 = available, >0 = in use (frames until recyclable)
    };
    std::vector<StagingBuffer> _stagingVertexPool;
    std::vector<StagingBuffer> _stagingIndexPool;
    std::vector<StagingBuffer> _stagingGenericPool; // PHASE 6: For chunk metadata and other uploads

    static constexpr VkDeviceSize STAGING_BUFFER_SIZE_VERTEX = 128 * 1024;  // 128KB per buffer
    static constexpr VkDeviceSize STAGING_BUFFER_SIZE_INDEX = 64 * 1024;    // 64KB per buffer
    static constexpr VkDeviceSize STAGING_BUFFER_SIZE_GENERIC = 16 * 1024;  // 16KB per buffer (PHASE 6)

    // Mark staging buffers as recyclable after frame delay
    void updateStagingPools();

  public:
    // PHASE 3: Submit all pending uploads in a single batch
    void submitPendingUploads(
        const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit);
};

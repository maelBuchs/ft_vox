#include "MeshBufferPool.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include <tracy/Tracy.hpp>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"

// PHASE 1 FIX: Increased from 512MB (128M indices) to 2GB (512M indices)
// For extreme view distances (40k+ chunks @ avg 8k triangles each):
//   - 40k chunks * 8k triangles * 3 indices = 960M indices = 3.8GB needed
//   - Pre-allocating 2GB reduces resize frequency from 4+ resizes to 1-2
//   - Each resize causes GPU stall (vkWaitForFences during buffer copy) = frame stutter
//   - Trade-off: Higher VRAM usage (2GB) for stable FPS on high-end GPUs
// TODO (Phase 3): Replace with async transfer queue to eliminate stutter entirely
constexpr VkDeviceSize INITIAL_INDEX_BUFFER_SIZE = 512ull * 1024 * 1024 * sizeof(uint32_t); // 2GB

MeshBufferPool::MeshBufferPool(VulkanDevice& device, VulkanBuffer& bufferManager)
    : _device(device), _bufferManager(bufferManager) {

    // Create mega index buffer (shared by all chunks) with initial capacity
    _currentIndexBufferSize = INITIAL_INDEX_BUFFER_SIZE;
    _indexBuffer = _bufferManager.createBuffer(
        _currentIndexBufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
}

MeshBufferPool::~MeshBufferPool() {
    auto start = std::chrono::high_resolution_clock::now();

    // Flush any remaining deletion queue (should be empty if VoxelRenderer cleaned up properly)
    flushDeletionQueue();

    // PHASE 4: Destroy all pooled staging buffers
    for (auto& staging : _stagingVertexPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    for (auto& staging : _stagingIndexPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    // PHASE 6: Destroy generic pool
    for (auto& staging : _stagingGenericPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    _stagingVertexPool.clear();
    _stagingIndexPool.clear();
    _stagingGenericPool.clear();

    // Destroy mega index buffer
    if (_indexBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indexBuffer);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "[MeshBufferPool] Destructor took: "
              << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
}

ChunkMeshBuffers MeshBufferPool::allocateChunkBuffers(
    std::span<uint32_t> indices, std::span<uint32_t> vertices,
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {
    ZoneScopedN("MeshBufferPool::allocateChunkBuffers");

    const size_t vertexSize = vertices.size_bytes();
    const size_t indexSize = indices.size_bytes();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(indices.size());

    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("[MeshBufferPool] Cannot allocate empty mesh!");
    }

    ChunkMeshBuffers chunkBuffers;
    chunkBuffers.vertexCount = vertexCount;
    chunkBuffers.indexCount = indexCount;

    // ========================================================================
    // ALLOCATE PER-CHUNK VERTEX BUFFER via VMA
    // ========================================================================
    chunkBuffers.vertexBuffer = _bufferManager.createBuffer(
        vertexSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Get buffer device address for shader access
    VkBufferDeviceAddressInfo vertexAddressInfo{};
    vertexAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    vertexAddressInfo.buffer = chunkBuffers.vertexBuffer.buffer;
    chunkBuffers.vertexAddress = vkGetBufferDeviceAddress(_device.getDevice(), &vertexAddressInfo);

    // ========================================================================
    // ALLOCATE INDEX SUB-ALLOCATION in mega buffer (with free-list recycling)
    // ========================================================================
    uint32_t indexOffset = 0;
    bool foundFreeRange = false;

    // Strategy 1: Try to find a suitable free range (first-fit)
    for (auto it = _freeIndexRanges.begin(); it != _freeIndexRanges.end(); ++it) {
        if (it->count >= indexCount) {
            // Found a suitable range!
            indexOffset = it->start;
            foundFreeRange = true;

            // If range is larger than needed, split it
            if (it->count > indexCount) {
                it->start += indexCount;
                it->count -= indexCount;
            } else {
                // Exact fit - remove from free list
                _freeIndexRanges.erase(it);
            }
            break;
        }
    }

    // Strategy 2: No free range found - allocate from tail
    if (!foundFreeRange) {
        indexOffset = _indexOffset;

        // Ensure we have enough capacity (will resize if needed)
        ensureIndexBufferCapacity(_indexOffset + indexCount, immediateSubmit);

        _indexOffset += indexCount;
    }

    chunkBuffers.firstIndex = indexOffset;

    // ========================================================================
    // PHASE 4: ACQUIRE FROM STAGING BUFFER POOL (reuse instead of create/destroy)
    // ========================================================================
    AllocatedBuffer stagingVertex = acquireStagingBuffer(vertexSize, StagingType::Vertex);
    AllocatedBuffer stagingIndex = acquireStagingBuffer(indexSize, StagingType::Index);

    try {
        _bufferManager.uploadToBuffer(stagingVertex, vertices.data(), vertexSize);
        _bufferManager.uploadToBuffer(stagingIndex, indices.data(), indexSize);

        // PHASE 3: Queue for batched upload instead of immediate submit
        PendingUpload upload{
            .stagingVertex = stagingVertex,
            .stagingIndex = stagingIndex,
            .dstVertexBuffer = chunkBuffers.vertexBuffer.buffer,
            .dstIndexBuffer = _indexBuffer.buffer,
            .vertexSize = vertexSize,
            .indexSize = indexSize,
            .indexOffset = indexOffset
        };
        _pendingUploads.push_back(upload);

        // NOTE: Staging buffers will be destroyed in submitPendingUploads()

    } catch (...) {
        // PHASE 4: Staging buffers are pooled - mark as available instead of destroying
        // (They will be recycled on next updateStagingPools call)

        // Clean up the vertex buffer we created
        _bufferManager.destroyBuffer(chunkBuffers.vertexBuffer);

        // Rollback index allocation
        if (foundFreeRange) {
            // Return the range we took from free list
            returnIndexRangeToFreeList(indexOffset, indexCount);
        } else {
            // Rollback tail allocation
            _indexOffset -= indexCount;
        }

        // Re-throw the exception
        throw;
    }

    // Update statistics
    _allocatedChunks++;
    _totalVertexMemory += vertexSize;
    _totalIndexMemory += indexSize;
    _totalAllocations++;

    return chunkBuffers;
}

void MeshBufferPool::freeChunkBuffers(const ChunkMeshBuffers& buffers) {
    ZoneScopedN("MeshBufferPool::freeChunkBuffers");
    if (buffers.vertexBuffer.buffer == VK_NULL_HANDLE) {
        return; // Already freed or never allocated
    }

    // CRITICAL: Don't destroy immediately! GPU might still be using it.
    // Add to deletion queue and process after GPU is done
    _deletionQueue.push_back(DeletionEntry{.vertexBuffer = buffers.vertexBuffer,
                                           .vertexCount = buffers.vertexCount,
                                           .frameDelay = FrameManager::FRAME_OVERLAP});

    // Queue index range for recycling (also with frame overlap delay)
    _pendingIndexFrees.push_back(PendingIndexFree{.start = buffers.firstIndex,
                                                  .count = buffers.indexCount,
                                                  .frameDelay = FrameManager::FRAME_OVERLAP});

    // Update statistics immediately (user visible)
    const size_t vertexSize = buffers.vertexCount * sizeof(uint32_t);
    const size_t indexSize = buffers.indexCount * sizeof(uint32_t);

    if (_allocatedChunks > 0) {
        _allocatedChunks--;
    }
    if (_totalVertexMemory >= vertexSize) {
        _totalVertexMemory -= vertexSize;
    }
    if (_totalIndexMemory >= indexSize) {
        _totalIndexMemory -= indexSize;
    }
    _totalFrees++;

    // Index ranges are queued for recycling (with 2-frame GPU safety delay)
}

void MeshBufferPool::processDeletionQueue() {
    ZoneScopedN("MeshBufferPool::processDeletionQueue");
    // Decrement frame delay and delete vertex buffers that are ready
    for (auto it = _deletionQueue.begin(); it != _deletionQueue.end();) {
        it->frameDelay--;

        if (it->frameDelay == 0) {
            // Safe to delete now - GPU is done with this vertex buffer
            _bufferManager.destroyBuffer(it->vertexBuffer);
            it = _deletionQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Also process pending index range frees
    processPendingIndexFrees();

    // PHASE 4: Update staging buffer pools (recycle buffers after GPU is done)
    updateStagingPools();
}

void MeshBufferPool::flushDeletionQueue() {
    // Force delete all pending vertex buffers (for shutdown)
    for (const auto& entry : _deletionQueue) {
        _bufferManager.destroyBuffer(entry.vertexBuffer);
    }
    _deletionQueue.clear();

    // Also flush pending index frees (return them to free list immediately)
    for (const auto& pending : _pendingIndexFrees) {
        returnIndexRangeToFreeList(pending.start, pending.count);
    }
    _pendingIndexFrees.clear();
}

void MeshBufferPool::processPendingIndexFrees() {
    // Process pending index frees with frame delay (same as vertex buffers)
    for (auto it = _pendingIndexFrees.begin(); it != _pendingIndexFrees.end();) {
        it->frameDelay--;

        if (it->frameDelay == 0) {
            // Safe to recycle now - GPU is done with this index range
            returnIndexRangeToFreeList(it->start, it->count);
            it = _pendingIndexFrees.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshBufferPool::returnIndexRangeToFreeList(uint32_t start, uint32_t count) {
    // Special case: if this range is at the tail, shrink _indexOffset instead
    if (start + count == _indexOffset) {
        _indexOffset = start;

        // Aggressive tail shrinking: check if we can merge with consecutive ranges at the new tail
        bool merged = true;
        while (merged && !_freeIndexRanges.empty()) {
            merged = false;
            for (auto it = _freeIndexRanges.begin(); it != _freeIndexRanges.end(); ++it) {
                if (it->start + it->count == _indexOffset) {
                    // Found a range that's now at the tail - shrink further
                    _indexOffset = it->start;
                    _freeIndexRanges.erase(it);
                    merged = true;
                    break;
                }
            }
        }
        return;
    }

    // Insert into free list (maintain sorted order by start index)
    auto insertPos = _freeIndexRanges.begin();
    while (insertPos != _freeIndexRanges.end() && insertPos->start < start) {
        ++insertPos;
    }

    // Check for merge with previous range
    if (insertPos != _freeIndexRanges.begin()) {
        auto prev = std::prev(insertPos);
        if (prev->start + prev->count == start) {
            // Merge with previous range
            prev->count += count;
            start = prev->start;
            count = prev->count;
            _freeIndexRanges.erase(prev);
            // Re-find insertion position after modification
            insertPos = _freeIndexRanges.begin();
            while (insertPos != _freeIndexRanges.end() && insertPos->start < start) {
                ++insertPos;
            }
        }
    }

    // Check for merge with next range
    if (insertPos != _freeIndexRanges.end() && start + count == insertPos->start) {
        // Merge with next range
        insertPos->start = start;
        insertPos->count += count;
        return;
    }

    // No merge possible - insert as new range
    _freeIndexRanges.insert(insertPos, IndexRange{start, count});
}

void MeshBufferPool::ensureIndexBufferCapacity(
    uint32_t requiredIndices,
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {

    const VkDeviceSize requiredBytes =
        static_cast<VkDeviceSize>(requiredIndices) * sizeof(uint32_t);

    // Check if we need to resize
    if (requiredBytes <= _currentIndexBufferSize) {
        return;
    }

    // Calculate new capacity with 1.5x growth factor
    VkDeviceSize newCapacity = static_cast<VkDeviceSize>(requiredBytes * 1.5);

    // Align to 256MB boundaries for cleaner allocation
    const VkDeviceSize alignment = 256ull * 1024 * 1024;
    newCapacity = ((newCapacity + alignment - 1) / alignment) * alignment;

    std::cout << "[MeshBufferPool] Resizing mega index buffer from "
              << (_currentIndexBufferSize / (1024 * 1024)) << " MB to "
              << (newCapacity / (1024 * 1024)) << " MB\n";

    // Create new larger buffer
    AllocatedBuffer newIndexBuffer = _bufferManager.createBuffer(
        newCapacity,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Copy existing data from old buffer to new buffer
    const VkDeviceSize bytesToCopy = static_cast<VkDeviceSize>(_indexOffset) * sizeof(uint32_t);
    if (bytesToCopy > 0) {
        immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = bytesToCopy;
            vkCmdCopyBuffer(cmd, _indexBuffer.buffer, newIndexBuffer.buffer, 1, &copyRegion);
        });
    }

    // Destroy old buffer (safe now because copy is complete)
    _bufferManager.destroyBuffer(_indexBuffer);

    // Replace with new buffer
    _indexBuffer = newIndexBuffer;
    _currentIndexBufferSize = newCapacity;
}

void MeshBufferPool::destroyAllChunkBuffers(const std::vector<ChunkMeshBuffers>& buffers) {
    // Fast shutdown path - destroy all buffers immediately (GPU already idle)
    std::cout << "[MeshBufferPool] Fast destroying " << buffers.size()
              << " chunk vertex buffers...\n";
    auto start = std::chrono::high_resolution_clock::now();

    size_t destroyedCount = 0;
    for (const auto& buf : buffers) {
        if (buf.vertexBuffer.buffer != VK_NULL_HANDLE) {
            _bufferManager.destroyBuffer(buf.vertexBuffer);
            destroyedCount++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "[MeshBufferPool] Batch destroy took: "
              << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";

    // Update tracking counters to prevent leak warnings
    _totalFrees += destroyedCount;
    _allocatedChunks = 0;
    _totalVertexMemory = 0;
    _totalIndexMemory = 0;
}

void MeshBufferPool::resetIndexOffset() {
    // CRITICAL: Only reset when no chunks are allocated!
    if (_allocatedChunks != 0) {
        std::cout << "[MeshBufferPool] WARNING: Cannot reset index offset - " << _allocatedChunks
                  << " chunks still allocated!\n";
        return;
    }

    _indexOffset = 0;
    _totalIndexMemory = 0;

    // Clear free lists (everything is reset)
    _freeIndexRanges.clear();
    _pendingIndexFrees.clear();
}

float MeshBufferPool::getMemoryUsage() const {
    // Return total GPU memory usage in MB (vertices + indices)
    const size_t totalBytes = _totalVertexMemory + _totalIndexMemory;
    return static_cast<float>(totalBytes) / (1024.0f * 1024.0f);
}

// ============================================================================
// PHASE 4: STAGING BUFFER POOL IMPLEMENTATION
// ============================================================================

// PHASE 6: Updated to support 3 pool types (Vertex, Index, Generic)
AllocatedBuffer MeshBufferPool::acquireStagingBuffer(VkDeviceSize requiredSize, StagingType type) {
    ZoneScopedN("MeshBufferPool::acquireStagingBuffer");

    // Select the appropriate pool and size based on type
    std::vector<StagingBuffer>* pool;
    VkDeviceSize poolBufferSize;

    switch (type) {
        case StagingType::Vertex:
            pool = &_stagingVertexPool;
            poolBufferSize = STAGING_BUFFER_SIZE_VERTEX;
            break;
        case StagingType::Index:
            pool = &_stagingIndexPool;
            poolBufferSize = STAGING_BUFFER_SIZE_INDEX;
            break;
        case StagingType::Generic:
            pool = &_stagingGenericPool;
            poolBufferSize = STAGING_BUFFER_SIZE_GENERIC;
            break;
    }

    // Try to find an available buffer that's large enough
    for (auto& staging : *pool) {
        if (staging.frameDelay == 0 && staging.size >= requiredSize) {
            // Found a reusable buffer!
            staging.frameDelay = FrameManager::FRAME_OVERLAP; // Mark as in-use
            return staging.buffer;
        }
    }

    // No suitable buffer found - create a new one
    // Use the larger of requiredSize or poolBufferSize for better reuse
    const VkDeviceSize allocSize = std::max(requiredSize, poolBufferSize);

    AllocatedBuffer newBuffer = _bufferManager.createBuffer(
        allocSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Add to pool for future reuse
    pool->push_back(StagingBuffer{
        .buffer = newBuffer,
        .size = allocSize,
        .frameDelay = FrameManager::FRAME_OVERLAP
    });

    return newBuffer;
}

void MeshBufferPool::updateStagingPools() {
    ZoneScopedN("MeshBufferPool::updateStagingPools");

    // Decrement frame delay for all in-use staging buffers
    for (auto& staging : _stagingVertexPool) {
        if (staging.frameDelay > 0) {
            staging.frameDelay--;
        }
    }

    for (auto& staging : _stagingIndexPool) {
        if (staging.frameDelay > 0) {
            staging.frameDelay--;
        }
    }

    // PHASE 6: Handle generic pool
    for (auto& staging : _stagingGenericPool) {
        if (staging.frameDelay > 0) {
            staging.frameDelay--;
        }
    }

    // Optional: Clean up excess buffers if pool grows too large (keep max 32 per type)
    // This prevents unbounded memory growth while still maintaining good reuse
    constexpr size_t MAX_POOL_SIZE = 32;

    auto cleanupPool = [this](std::vector<StagingBuffer>& pool) {
        if (pool.size() > MAX_POOL_SIZE) {
            // Remove oldest available buffers (frameDelay == 0)
            size_t removed = 0;
            for (auto it = pool.begin(); it != pool.end() && pool.size() > MAX_POOL_SIZE;) {
                if (it->frameDelay == 0) {
                    _bufferManager.destroyBuffer(it->buffer);
                    it = pool.erase(it);
                    removed++;
                } else {
                    ++it;
                }
            }
        }
    };

    cleanupPool(_stagingVertexPool);
    cleanupPool(_stagingIndexPool);
    cleanupPool(_stagingGenericPool); // PHASE 6
}

// PHASE 3: Submit all pending uploads in a single batch
void MeshBufferPool::submitPendingUploads(
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {

    if (_pendingUploads.empty()) {
        return; // Nothing to upload
    }

    ZoneScopedN("MeshBufferPool::submitPendingUploads");

    // PHASE 3+7: Batch all copy commands into a single command buffer
    // Caller (VoxelRenderer) provides submit function (async transfer or immediate)
    immediateSubmit([this](VkCommandBuffer cmd) {
        for (const auto& upload : _pendingUploads) {
            // Copy vertex data to per-chunk buffer
            VkBufferCopy vertexCopy{};
            vertexCopy.srcOffset = 0;
            vertexCopy.dstOffset = 0;
            vertexCopy.size = upload.vertexSize;
            vkCmdCopyBuffer(cmd, upload.stagingVertex.buffer, upload.dstVertexBuffer, 1, &vertexCopy);

            // Copy index data to mega buffer at allocated offset
            VkBufferCopy indexCopy{};
            indexCopy.srcOffset = 0;
            indexCopy.dstOffset = upload.indexOffset * sizeof(uint32_t);
            indexCopy.size = upload.indexSize;
            vkCmdCopyBuffer(cmd, upload.stagingIndex.buffer, upload.dstIndexBuffer, 1, &indexCopy);
        }
    });

    // PHASE 4: Staging buffers are NOT destroyed - they're pooled and will be recycled
    // The frameDelay counter was already set in acquireStagingBuffer()
    // updateStagingPools() will decrement the delay each frame and mark them available

    size_t uploadCount = _pendingUploads.size();
    _pendingUploads.clear();

    std::cout << "[MeshBufferPool] PHASE 3+4: Batched " << uploadCount
              << " uploads (staging buffers pooled)\n";
}

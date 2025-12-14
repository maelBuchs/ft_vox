#include "MeshBufferPool.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <tracy/Tracy.hpp>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"

// Start with conservative 64MB initial size and grow dynamically as needed
// Typical startup loads ~500-1000 chunks (10-20MB of indices)
// 64MB provides headroom without wasting 2GB upfront
// Buffer grows automatically via ensureIndexBufferCapacity() when needed
constexpr VkDeviceSize INITIAL_INDEX_BUFFER_SIZE = 16ull * 1024 * 1024 * sizeof(uint32_t);

UploadRingBuffer::UploadRingBuffer(VulkanDevice& device, VulkanBuffer& bufferManager)
    : _device(device), _bufferManager(bufferManager) {

    // Create 256MB ring buffer with HOST_VISIBLE | HOST_COHERENT memory
    _ringBuffer = _bufferManager.createBuffer(
        RING_SIZE,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // Used as source for GPU transfers
        VMA_MEMORY_USAGE_CPU_ONLY);       // CPU-writeable, auto-mapped by VMA

    // Get persistently mapped pointer from VMA (automatically mapped)
    _mappedPtr = _ringBuffer.info.pMappedData;
    if (!_mappedPtr) {
        throw std::runtime_error("[UploadRingBuffer] Failed to get mapped pointer from VMA");
    }

    for (auto& frame : _frameStates) {
        frame.startOffset = 0;
        frame.endOffset = 0;
    }
}

UploadRingBuffer::~UploadRingBuffer() {
    // VMA will automatically unmap when we destroy the buffer
    _bufferManager.destroyBuffer(_ringBuffer);
}

UploadRingBuffer::Allocation UploadRingBuffer::allocate(VkDeviceSize size, uint32_t frameIndex) {
    // Align size to 256 bytes (common buffer alignment requirement)
    const VkDeviceSize alignment = 256;
    const VkDeviceSize alignedSize = (size + alignment - 1) & ~(alignment - 1);

    // Atomic fetch-add to get allocation offset (thread-safe!)
    VkDeviceSize offset = _writeOffset.fetch_add(alignedSize, std::memory_order_relaxed);

    // Check if we've wrapped around the ring
    if (offset + alignedSize > RING_SIZE) {
        // Ring buffer full - rollback the atomic add and signal caller to flush
        _writeOffset.fetch_sub(alignedSize, std::memory_order_relaxed);
        return Allocation{.buffer = VK_NULL_HANDLE, .offset = 0, .mappedPtr = nullptr, .size = 0};
    }

    // Update frame end offset
    const uint32_t frameIdx = frameIndex % FRAME_COUNT;
    _frameStates[frameIdx].endOffset = offset + alignedSize;

    return Allocation{
        .buffer = _ringBuffer.buffer,
        .offset = offset,
        .mappedPtr = static_cast<char*>(_mappedPtr) + offset,
        .size = alignedSize
    };
}

void UploadRingBuffer::frameComplete(uint32_t frameIndex) {
    const uint32_t frameIdx = frameIndex % FRAME_COUNT;
    // Update this frame's region as complete
    _frameStates[frameIdx].startOffset = _frameStates[frameIdx].endOffset;

    // Check if all frames are complete (no in-flight data)
    bool allComplete = true;
    for (const auto& frame : _frameStates) {
        if (frame.startOffset != frame.endOffset) {
            allComplete = false;
            break;
        }
    }

    if (allComplete) {
        // All frames complete - reset ring to beginning
        _writeOffset.store(0, std::memory_order_relaxed);
        for (auto& frame : _frameStates) {
            frame.startOffset = 0;
            frame.endOffset = 0;
        }
        _frameStartOffset = 0;
    }
}

void UploadRingBuffer::reset() {
    _writeOffset.store(0, std::memory_order_relaxed);
    for (auto& frame : _frameStates) {
        frame.startOffset = 0;
        frame.endOffset = 0;
    }
    _frameStartOffset = 0;
}

float UploadRingBuffer::getUtilization() const {
    VkDeviceSize currentOffset = _writeOffset.load(std::memory_order_relaxed);
    return static_cast<float>(currentOffset) / static_cast<float>(RING_SIZE);
}

MeshBufferPool::MeshBufferPool(VulkanDevice& device, VulkanBuffer& bufferManager)
    : _device(device), _bufferManager(bufferManager) {

    _uploadRing = std::make_unique<UploadRingBuffer>(_device, _bufferManager);

    // Create mega index buffer (shared by all chunks) with initial capacity
    _currentIndexBufferSize = INITIAL_INDEX_BUFFER_SIZE;
    _indexBuffer = _bufferManager.createBuffer(
        _currentIndexBufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
}

MeshBufferPool::~MeshBufferPool() {
    flushDeletionQueue();

    for (auto& staging : _stagingVertexPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    for (auto& staging : _stagingIndexPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    for (auto& staging : _stagingGenericPool) {
        _bufferManager.destroyBuffer(staging.buffer);
    }
    _stagingVertexPool.clear();
    _stagingIndexPool.clear();
    _stagingGenericPool.clear();

    if (_indexBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indexBuffer);
    }
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

    // Allocate per-chunk vertex buffer via VMA
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

    // Allocate index sub-allocation in mega buffer (with free-list recycling)
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

        // Submit any pending uploads before resizing the buffer
        // Otherwise pending uploads will reference the old (soon-to-be-destroyed) buffer
        if (!_pendingUploads.empty()) {
            submitPendingUploads(immediateSubmit);
        }

        // Ensure we have enough capacity (will resize if needed)
        ensureIndexBufferCapacity(_indexOffset + indexCount, immediateSubmit);

        _indexOffset += indexCount;
    }

    chunkBuffers.firstIndex = indexOffset;

    auto vertexAlloc = _uploadRing->allocate(vertexSize, _currentFrameIndex);
    auto indexAlloc = _uploadRing->allocate(indexSize, _currentFrameIndex);

    // Check if ring buffer allocation failed
    if (vertexAlloc.buffer == VK_NULL_HANDLE || indexAlloc.buffer == VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(chunkBuffers.vertexBuffer);

        if (foundFreeRange) {
            returnIndexRangeToFreeList(indexOffset, indexCount);
        } else {
            _indexOffset -= indexCount;
        }

        return ChunkMeshBuffers{};
    }

    try {
        std::memcpy(vertexAlloc.mappedPtr, vertices.data(), vertexSize);
        std::memcpy(indexAlloc.mappedPtr, indices.data(), indexSize);

        PendingUpload upload{
            .stagingVertex = AllocatedBuffer{vertexAlloc.buffer, {}, {}}, // Ring buffer handle
            .stagingIndex = AllocatedBuffer{indexAlloc.buffer, {}, {}},   // Ring buffer handle
            .dstVertexBuffer = chunkBuffers.vertexBuffer.buffer,
            .dstIndexBuffer = _indexBuffer.buffer,
            .vertexSize = vertexSize,
            .indexSize = indexSize,
            .indexOffset = indexOffset,
            .vertexSrcOffset = vertexAlloc.offset,
            .indexSrcOffset = indexAlloc.offset
        };
        _pendingUploads.push_back(upload);

    } catch (...) {
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

    // Don't destroy immediately - GPU might still be using it
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

    processPendingIndexFrees();

    updateStagingPools();

    _uploadRing->frameComplete(_currentFrameIndex);
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

    const VkDeviceSize alignment = 256ull * 1024 * 1024;
    newCapacity = ((newCapacity + alignment - 1) / alignment) * alignment;

    vkDeviceWaitIdle(_device.getDevice());

    // Store old buffer handle for copying
    VkBuffer oldBuffer = _indexBuffer.buffer;
    const VkDeviceSize bytesToCopy = static_cast<VkDeviceSize>(_indexOffset) * sizeof(uint32_t);

    // Create new larger buffer
    AllocatedBuffer newIndexBuffer = _bufferManager.createBuffer(
        newCapacity,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Verify buffer was created successfully
    if (newIndexBuffer.buffer == VK_NULL_HANDLE) {
        throw std::runtime_error("[MeshBufferPool] Failed to create resized index buffer");
    }

    // Copy existing data from old buffer to new buffer
    if (bytesToCopy > 0) {
        // Capture buffer handles by value, not by reference to local variables
        VkBuffer srcBuffer = oldBuffer;
        VkBuffer dstBuffer = newIndexBuffer.buffer;

        immediateSubmit([srcBuffer, dstBuffer, bytesToCopy](VkCommandBuffer cmd) {
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = bytesToCopy;
            vkCmdCopyBuffer(cmd, srcBuffer, dstBuffer, 1, &copyRegion);
        });
    }

    // Destroy old buffer (safe now because copy is complete and GPU is idle)
    _bufferManager.destroyBuffer(_indexBuffer);

    // Replace with new buffer
    _indexBuffer = newIndexBuffer;
    _currentIndexBufferSize = newCapacity;

    // Notify listeners (e.g., VoxelRenderer) to update descriptor sets
    if (_indexBufferResizeCallback) {
        _indexBufferResizeCallback();
    }
}

void MeshBufferPool::destroyAllChunkBuffers(const std::vector<ChunkMeshBuffers>& buffers) {
    size_t destroyedCount = 0;
    for (const auto& buf : buffers) {
        if (buf.vertexBuffer.buffer != VK_NULL_HANDLE) {
            _bufferManager.destroyBuffer(buf.vertexBuffer);
            destroyedCount++;
        }
    }

    _totalFrees += destroyedCount;
    _allocatedChunks = 0;
    _totalVertexMemory = 0;
    _totalIndexMemory = 0;
}

void MeshBufferPool::resetIndexOffset() {
    if (_allocatedChunks != 0) {
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
    cleanupPool(_stagingGenericPool);
}

void MeshBufferPool::submitPendingUploads(
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {

    if (_pendingUploads.empty()) {
        return; // Nothing to upload
    }

    ZoneScopedN("MeshBufferPool::submitPendingUploads");

    immediateSubmit([this](VkCommandBuffer cmd) {
        for (const auto& upload : _pendingUploads) {
            VkBufferCopy vertexCopy{};
            vertexCopy.srcOffset = upload.vertexSrcOffset;
            vertexCopy.dstOffset = 0;
            vertexCopy.size = upload.vertexSize;
            vkCmdCopyBuffer(cmd, upload.stagingVertex.buffer, upload.dstVertexBuffer, 1, &vertexCopy);

            if (upload.dstIndexBuffer != _indexBuffer.buffer) {
                throw std::runtime_error(
                    "[MeshBufferPool] CRITICAL: Pending upload references stale index buffer! "
                    "This should never happen - uploads should be flushed before resize.");
            }

            VkBufferCopy indexCopy{};
            indexCopy.srcOffset = upload.indexSrcOffset;
            indexCopy.dstOffset = upload.indexOffset * sizeof(uint32_t);
            indexCopy.size = upload.indexSize;
            vkCmdCopyBuffer(cmd, upload.stagingIndex.buffer, upload.dstIndexBuffer, 1, &indexCopy);
        }
    });

    _pendingUploads.clear();

    _uploadRing->reset();
}

#include "ChunkBufferManager.hpp"

#include <algorithm>
#include <stdexcept>

#include <tracy/Tracy.hpp>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
#include "../Renderer.hpp"
#include "../Rendering/CommandExecutor.hpp"
#include "VoxelPipelineManager.hpp"
#include "common/World/Chunk.hpp"

ChunkBufferManager::ChunkBufferManager(
    VulkanDevice& device, VulkanBuffer& bufferManager,
    DescriptorAllocatorGrowable& descriptorAllocator, CommandExecutor& executor,
    Renderer& renderer, VoxelPipelineManager& pipelineManager,
    std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues)
    : _device(device), _bufferManager(bufferManager), _descriptorAllocator(descriptorAllocator),
      _executor(executor), _renderer(renderer), _pipelineManager(pipelineManager),
      _perThreadMeshQueues(perThreadMeshQueues) {
    _meshPool = std::make_unique<MeshBufferPool>(_device, _bufferManager);
}

ChunkBufferManager::~ChunkBufferManager() {
    vkDeviceWaitIdle(_device.getDevice());

    if (_meshPool) {
        _meshPool->flushDeletionQueue();
    }

    // Extract mesh buffers for batch destruction
    std::vector<ChunkMeshBuffers> allBuffers;
    allBuffers.reserve(_chunkDrawInfos.size());
    for (const auto& info : _chunkDrawInfos) {
        if (info.meshBuffers.vertexBuffer.buffer != VK_NULL_HANDLE &&
            info.meshBuffers.indexCount > 0) {
            allBuffers.push_back(info.meshBuffers);
        }
    }

    if (!allBuffers.empty() && _meshPool) {
        _meshPool->destroyAllChunkBuffers(allBuffers);
    }

    _chunkDrawInfos.clear();
    _chunkDrawLookup.clear();

    // Clean up buffers
    if (_indirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indirectBuffer);
    }
    for (auto& buffer : _chunkDataBuffers) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            _bufferManager.destroyBuffer(buffer);
        }
    }
    if (_atlasConfigBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_atlasConfigBuffer);
    }
    for (auto& buffer : _cameraUniformBuffers) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            _bufferManager.destroyBuffer(buffer);
        }
    }
    if (_frustumUniformBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_frustumUniformBuffer);
    }
    if (_culledIndirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_culledIndirectBuffer);
    }
    if (_culledChunkDataBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_culledChunkDataBuffer);
    }
}

void ChunkBufferManager::init(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow) {
    ZoneScoped;

    // Create indirect buffer
    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Create chunk data buffers
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDataBuffers[i] = _bufferManager.createBuffer(
            sizeof(GPUChunkData) * _currentMaxChunks,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // Create atlas config buffer
    _atlasConfigBuffer = _bufferManager.createBuffer(
        sizeof(int), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    _bufferManager.uploadToBuffer(_atlasConfigBuffer, &texturesPerRow, sizeof(int));

    // Initialize compute culling buffers
    initComputeCullingBuffers();

    // Initialize mesh shader camera buffer if supported
    if (_pipelineManager.supportsMeshShaders()) {
        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            _cameraUniformBuffers[i] = _bufferManager.createBuffer(
                sizeof(GPUCameraData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
        }

        // Set up index buffer resize callback
        _meshPool->setIndexBufferResizeCallback([this]() {
            _indexBufferResizePending = true;
        });
    }

    // Allocate descriptor sets
    allocateDescriptorSets(atlasView, atlasSampler);

    // Initialize mesh shader descriptors if supported
    if (_pipelineManager.supportsMeshShaders()) {
        initMeshShaderDescriptors(atlasView, atlasSampler);
    }
}

void ChunkBufferManager::initComputeCullingBuffers() {
    ZoneScoped;

    _frustumUniformBuffer = _bufferManager.createBuffer(
        sizeof(GPUFrustumData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    const size_t indirectBufferSize =
        sizeof(uint32_t) + (sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks);
    _culledIndirectBuffer = _bufferManager.createBuffer(
        indirectBufferSize,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    _culledChunkDataBuffer = _bufferManager.createBuffer(
        sizeof(GPUChunkData) * _currentMaxChunks,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
}

void ChunkBufferManager::allocateDescriptorSets(VkImageView atlasView, VkSampler atlasSampler) {
    ZoneScoped;

    // Allocate chunk descriptor sets
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDescriptorSets[i] = _descriptorAllocator.allocate(
            _device.getDevice(), _pipelineManager.getChunkSetLayout(), nullptr);

        DescriptorWriter writer;
        writer.writeBuffer(0, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * _currentMaxChunks,
                           0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.writeBuffer(2, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        writer.updateSet(_device.getDevice(), _chunkDescriptorSets[i]);
    }

    // Allocate traditional fragment descriptor set
    _traditionalFragDescriptorSet = _descriptorAllocator.allocate(
        _device.getDevice(), _pipelineManager.getTraditionalFragSetLayout(), nullptr);

    DescriptorWriter fragWriter;
    fragWriter.writeImage(0, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    fragWriter.writeBuffer(1, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    fragWriter.updateSet(_device.getDevice(), _traditionalFragDescriptorSet);

    // Allocate frustum cull descriptor set
    _frustumCullDescriptorSet = _descriptorAllocator.allocate(
        _device.getDevice(), _pipelineManager.getFrustumCullSetLayout(), nullptr);

    const size_t indirectBufferSize =
        sizeof(uint32_t) + (sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks);

    DescriptorWriter computeWriter;
    computeWriter.writeBuffer(0, _chunkDataBuffers[0].buffer,
                              sizeof(GPUChunkData) * _currentMaxChunks, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    computeWriter.writeBuffer(1, _frustumUniformBuffer.buffer, sizeof(GPUFrustumData), 0,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    computeWriter.writeBuffer(2, _culledIndirectBuffer.buffer, indirectBufferSize, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    computeWriter.writeBuffer(3, _culledChunkDataBuffer.buffer,
                              sizeof(GPUChunkData) * _currentMaxChunks, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    computeWriter.updateSet(_device.getDevice(), _frustumCullDescriptorSet);

    // Allocate culled chunk descriptor set (for drawing after compute culling)
    _culledChunkDescriptorSet = _descriptorAllocator.allocate(
        _device.getDevice(), _pipelineManager.getChunkSetLayout(), nullptr);

    DescriptorWriter culledWriter;
    culledWriter.writeBuffer(0, _culledChunkDataBuffer.buffer,
                             sizeof(GPUChunkData) * _currentMaxChunks, 0,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    culledWriter.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    culledWriter.writeBuffer(2, _atlasConfigBuffer.buffer, sizeof(int), 0,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    culledWriter.updateSet(_device.getDevice(), _culledChunkDescriptorSet);
}

void ChunkBufferManager::initMeshShaderDescriptors(VkImageView atlasView, VkSampler atlasSampler) {
    ZoneScoped;

    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _meshShaderDescriptorSets[i] = _descriptorAllocator.allocate(
            _device.getDevice(), _pipelineManager.getMeshShaderSetLayout(), nullptr);

        DescriptorWriter meshWriter;
        meshWriter.writeBuffer(0, _cameraUniformBuffers[i].buffer, sizeof(GPUCameraData), 0,
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        meshWriter.writeBuffer(1, _chunkDataBuffers[i].buffer,
                               sizeof(GPUChunkData) * _currentMaxChunks, 0,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // Binding 2 (index buffer) will be written after init
        meshWriter.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
    }

    // Write index buffer binding now
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        DescriptorWriter indexWriter;
        indexWriter.writeBuffer(2, _meshPool->getIndexBuffer(),
                                _meshPool->getIndexBufferCapacity(), 0,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        indexWriter.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
    }

    // Allocate fragment descriptor set
    _meshShaderFragDescriptorSet = _descriptorAllocator.allocate(
        _device.getDevice(), _pipelineManager.getMeshShaderFragSetLayout(), nullptr);

    DescriptorWriter fragWriter;
    fragWriter.writeImage(0, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    fragWriter.writeBuffer(1, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    fragWriter.updateSet(_device.getDevice(), _meshShaderFragDescriptorSet);
}

void ChunkBufferManager::ensureBufferCapacity(uint32_t requiredChunks) {
    if (requiredChunks <= _currentMaxChunks) {
        return;
    }

    // Schedule resize for next frame start (when GPU is synchronized via fence)
    // This avoids vkDeviceWaitIdle which stalls the entire GPU pipeline
    _pendingNewCapacity = static_cast<uint32_t>(requiredChunks * 1.5f);
    _bufferResizePending = true;
}

void ChunkBufferManager::executeBufferResize() {
    if (!_bufferResizePending || _pendingNewCapacity == 0) {
        return;
    }

    ZoneScopedN("Execute Deferred Buffer Resize");

    const uint32_t newCapacity = _pendingNewCapacity;

    // Destroy old buffers (safe because GPU finished via fence in beginFrame)
    _bufferManager.destroyBuffer(_indirectBuffer);
    for (auto& buffer : _chunkDataBuffers) {
        _bufferManager.destroyBuffer(buffer);
    }

    // Create new larger buffers
    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * newCapacity,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDataBuffers[i] = _bufferManager.createBuffer(
            sizeof(GPUChunkData) * newCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // Update descriptor sets
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        DescriptorWriter writer;
        writer.writeBuffer(0, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * newCapacity, 0,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(_device.getDevice(), _chunkDescriptorSets[i]);
    }

    if (_pipelineManager.supportsMeshShaders()) {
        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter writer;
            writer.writeBuffer(1, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * newCapacity,
                               0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }
    }

    _currentMaxChunks = newCapacity;

    // Mark all chunks as dirty for re-upload
    _dirtyChunkIndices.clear();
    _dirtyChunkIndices.resize(_chunkDrawInfos.size(), true);
    _dirtyChunkList.clear();
    for (size_t i = 0; i < _chunkDrawInfos.size(); i++) {
        _dirtyChunkList.push_back(static_cast<uint32_t>(i));
    }
    _chunkDataDirty = true;
    _dirtyChunkCount = static_cast<uint32_t>(_chunkDrawInfos.size());

    // Reset pending state
    _bufferResizePending = false;
    _pendingNewCapacity = 0;
}

void ChunkBufferManager::beginFrame() {
    ZoneScoped;

    const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
    _meshPool->setCurrentFrameIndex(frameIndex);

    {
        ZoneScopedN("Process Deletion Queue");
        _meshPool->processDeletionQueue();
    }

    // Execute pending buffer resize (GPU is synchronized via fence at frame start)
    if (_bufferResizePending) {
        executeBufferResize();
    }

    // Handle index buffer resize without vkDeviceWaitIdle
    // The fence wait at frame start guarantees GPU finished using old descriptors
    if (_indexBufferResizePending && _pipelineManager.supportsMeshShaders()) {
        ZoneScopedN("Process Index Buffer Resize");

        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter writer;
            writer.writeBuffer(2, _meshPool->getIndexBuffer(),
                               _meshPool->getIndexBufferCapacity(), 0,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }

        _indexBufferResizePending = false;
    }
}

void ChunkBufferManager::update(const glm::ivec3& cameraChunkPos, int maxLoadDistance) {
    ZoneScoped;
    _maxLoadDistance = maxLoadDistance;

    // Adaptive batch sizing
    {
        const auto now = std::chrono::steady_clock::now();
        const float frameTimeMs =
            std::chrono::duration<float, std::milli>(now - _lastFrameTime).count();
        _lastFrameTime = now;

        _avgFrameTimeMs = _avgFrameTimeMs * 0.95f + frameTimeMs * 0.05f;

        size_t totalPending = 0;
        for (const auto& queue : _perThreadMeshQueues) {
            totalPending += queue->size();
        }

        if (_avgFrameTimeMs < 10.0f && totalPending > 500) {
            _adaptiveMaxMeshes = 512;
            _adaptiveMinChunks = 8;
            _adaptiveMaxMs = 8;
        } else if (_avgFrameTimeMs < 16.67f && totalPending > 100) {
            _adaptiveMaxMeshes = 384;
            _adaptiveMinChunks = 16;
            _adaptiveMaxMs = 12;
        } else if (_avgFrameTimeMs > 25.0f) {
            _adaptiveMaxMeshes = 128;
            _adaptiveMinChunks = 64;
            _adaptiveMaxMs = 24;
        } else {
            _adaptiveMaxMeshes = MAX_MESHES_PER_BATCH;
            _adaptiveMinChunks = MIN_CHUNKS_FOR_UPLOAD;
            _adaptiveMaxMs = MAX_MS_BETWEEN_UPLOADS;
        }
    }

    // Reuse member vector instead of allocating each frame
    _meshBatch.clear();
    _meshBatch.reserve(_adaptiveMaxMeshes);

    // First, add any meshes that failed to upload last frame (ring buffer was full)
    if (!_retryMeshBatch.empty()) {
        ZoneScopedN("Process Retry Mesh Batch");
        const size_t retryCount = std::min(_retryMeshBatch.size(), static_cast<size_t>(_adaptiveMaxMeshes));
        _meshBatch.insert(_meshBatch.end(),
            std::make_move_iterator(_retryMeshBatch.begin()),
            std::make_move_iterator(_retryMeshBatch.begin() + static_cast<ptrdiff_t>(retryCount)));
        _retryMeshBatch.erase(_retryMeshBatch.begin(),
            _retryMeshBatch.begin() + static_cast<ptrdiff_t>(retryCount));
    }

    {
        ZoneScopedN("Collect Mesh Batch from All Queues");
        for (auto& queue : _perThreadMeshQueues) {
            const size_t remaining = _adaptiveMaxMeshes - _meshBatch.size();
            if (remaining == 0) {
                break;
            }
            queue->try_pop_batch(_meshBatch, remaining);
            if (_meshBatch.size() >= _adaptiveMaxMeshes) {
                break;
            }
        }
    }

    if (!_meshBatch.empty()) {
        ZoneScopedN("Process Mesh Batch");
        for (auto& meshData : _meshBatch) {
            const glm::ivec3 chunkCoords = meshData.chunkPosition;

            const glm::ivec3 offset = chunkCoords - cameraChunkPos;
            const int chebyshevDistance =
                std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

            if (chebyshevDistance > maxLoadDistance) {
                continue;
            }

            if (meshData.vertices.empty() || meshData.indices.empty()) {
                auto it = _chunkDrawLookup.find(chunkCoords);
                if (it != _chunkDrawLookup.end()) {
                    const size_t idx = it->second;
                    _meshPool->freeChunkBuffers(_chunkDrawInfos[idx].meshBuffers);

                    if (idx != _chunkDrawInfos.size() - 1) {
                        _chunkDrawInfos[idx] = _chunkDrawInfos.back();
                        _chunkDrawLookup[_chunkDrawInfos[idx].chunkCoords] = idx;
                    }
                    _chunkDrawInfos.pop_back();
                    _chunkDrawLookup.erase(it);
                    _chunkDataDirty = true;
                    _dirtyChunkCount++;
                }
                continue;
            }

            ChunkMeshBuffers chunkBuffers;
            {
                ZoneScopedN("Allocate Chunk Buffers");
                chunkBuffers = _meshPool->allocateChunkBuffers(
                    meshData.indices, meshData.vertices,
                    [this](std::function<void(VkCommandBuffer)>&& func) {
                        _executor.immediateSubmit(std::move(func));
                    });
            }

            if (chunkBuffers.vertexBuffer.buffer == VK_NULL_HANDLE) {
                _retryMeshBatch.push_back(std::move(meshData));
                continue;
            }

            const glm::vec3 chunkWorldPos{
                static_cast<float>(chunkCoords[0] * Chunk::CHUNK_SIZE),
                static_cast<float>(chunkCoords[1] * Chunk::CHUNK_SIZE),
                static_cast<float>(chunkCoords[2] * Chunk::CHUNK_SIZE)};

            if (auto it = _chunkDrawLookup.find(chunkCoords); it != _chunkDrawLookup.end()) {
                const size_t chunkIndex = it->second;
                ChunkDrawInfo& info = _chunkDrawInfos[chunkIndex];
                _meshPool->freeChunkBuffers(info.meshBuffers);

                info.chunkCoords = chunkCoords;
                info.worldPosition = chunkWorldPos;
                info.meshBuffers = chunkBuffers;
                _chunkDataDirty = true;
                _dirtyChunkCount++;

                if (chunkIndex >= _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices.resize(chunkIndex + 1, false);
                }
                if (!_dirtyChunkIndices[chunkIndex]) {
                    _dirtyChunkIndices[chunkIndex] = true;
                    _dirtyChunkList.push_back(static_cast<uint32_t>(chunkIndex));
                }
            } else {
                size_t reuseIndex = _chunkDrawInfos.size();
                for (size_t i = 0; i < _chunkDrawInfos.size(); i++) {
                    if (_chunkDrawInfos[i].meshBuffers.indexCount == 0) {
                        reuseIndex = i;
                        break;
                    }
                }

                size_t chunkIndex;
                if (reuseIndex < _chunkDrawInfos.size()) {
                    ChunkDrawInfo& info = _chunkDrawInfos[reuseIndex];
                    info.chunkCoords = chunkCoords;
                    info.worldPosition = chunkWorldPos;
                    info.meshBuffers = chunkBuffers;
                    _chunkDrawLookup.emplace(chunkCoords, reuseIndex);
                    chunkIndex = reuseIndex;
                } else {
                    chunkIndex = _chunkDrawInfos.size();
                    _chunkDrawInfos.push_back(ChunkDrawInfo{.chunkCoords = chunkCoords,
                                                            .worldPosition = chunkWorldPos,
                                                            .meshBuffers = chunkBuffers});
                    _chunkDrawLookup.emplace(chunkCoords, chunkIndex);
                }
                _chunkDataDirty = true;
                _dirtyChunkCount++;

                if (chunkIndex >= _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices.resize(chunkIndex + 1, false);
                }
                if (!_dirtyChunkIndices[chunkIndex]) {
                    _dirtyChunkIndices[chunkIndex] = true;
                    _dirtyChunkList.push_back(static_cast<uint32_t>(chunkIndex));
                }
            }
        }
    }

    if (!_meshBatch.empty()) {
        ZoneScopedN("Submit Batched Uploads");
        _meshPool->submitPendingUploads([this](std::function<void(VkCommandBuffer)>&& func) {
            _executor.immediateSubmit(std::move(func));
        });
    }

    // Throttled GPU upload for mesh shader path
    const auto now = std::chrono::steady_clock::now();
    const auto msSinceLastUpload =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastUploadTime).count();

    const bool shouldUpload = _chunkDataDirty && _pipelineManager.supportsMeshShaders() &&
                              (_dirtyChunkCount >= _adaptiveMinChunks ||
                               msSinceLastUpload >= _adaptiveMaxMs);

    if (shouldUpload) {
        ZoneScopedN("Upload Chunk Data (GPU-Persistent)");

        const uint32_t frameIndex =
            static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);

        // Check if we need more capacity
        const uint32_t requiredChunks = static_cast<uint32_t>(_chunkDrawInfos.size());
        if (requiredChunks > _currentMaxChunks) {
            // We need to resize - schedule it and skip this upload
            // The resize will happen at next beginFrame() when GPU is synchronized
            ensureBufferCapacity(requiredChunks);
            // Don't upload now - buffer is too small. Will upload next frame after resize.
            return;
        }

        _chunkDrawData.clear();
        _chunkDrawData.reserve(_chunkDrawInfos.size());

        for (const ChunkDrawInfo& drawInfo : _chunkDrawInfos) {
            const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;

            GPUChunkData chunkData{};
            chunkData.chunkWorldPos = drawInfo.worldPosition;
            chunkData.indexCount = buffers.indexCount;
            chunkData.vertexBufferAddress = buffers.vertexAddress;
            chunkData.firstIndex = buffers.firstIndex;
            chunkData._padding = 0;
            _chunkDrawData.push_back(chunkData);
        }

        if (!_chunkDrawData.empty()) {
            if (!_dirtyChunkList.empty() && _dirtyChunkList.size() < _chunkDrawData.size()) {
                ZoneScopedN("Upload Dirty Chunks Only (Batched)");

                // Build copy regions and dirty data together with same filter
                // This ensures srcOffset matches the position in _dirtyDataBuffer
                const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(_currentMaxChunks) * sizeof(GPUChunkData);

                _dirtyDataBuffer.clear();
                _dirtyDataBuffer.reserve(_dirtyChunkList.size());
                _copyRegions.clear();
                _copyRegions.reserve(_dirtyChunkList.size());

                for (uint32_t idx : _dirtyChunkList) {
                    // Skip if index is out of bounds for either data or GPU buffer
                    if (idx >= _chunkDrawData.size() || idx >= _currentMaxChunks) {
                        continue;
                    }

                    const VkDeviceSize dstOffset = static_cast<VkDeviceSize>(idx) * sizeof(GPUChunkData);

                    // Final safety check: ensure we don't write past buffer end
                    if (dstOffset + sizeof(GPUChunkData) > bufferSize) {
                        continue;
                    }

                    // Add data and corresponding copy region
                    VkBufferCopy copyRegion{};
                    copyRegion.srcOffset = _dirtyDataBuffer.size() * sizeof(GPUChunkData);
                    copyRegion.dstOffset = dstOffset;
                    copyRegion.size = sizeof(GPUChunkData);

                    _dirtyDataBuffer.push_back(_chunkDrawData[idx]);
                    _copyRegions.push_back(copyRegion);
                }

                // Only proceed if we have valid data to copy
                if (!_dirtyDataBuffer.empty() && !_copyRegions.empty()) {
                    const VkDeviceSize stagingSize = _dirtyDataBuffer.size() * sizeof(GPUChunkData);
                    AllocatedBuffer stagingBuffer = _meshPool->acquireStagingBuffer(
                        stagingSize, MeshBufferPool::StagingType::Generic);

                    _bufferManager.uploadToBuffer(stagingBuffer, _dirtyDataBuffer.data(),
                                                  _dirtyDataBuffer.size() * sizeof(GPUChunkData));

                    // Single batched copy command instead of N individual copies
                    _executor.immediateSubmit([&](VkCommandBuffer cmd) {
                        vkCmdCopyBuffer(cmd, stagingBuffer.buffer,
                                        _chunkDataBuffers[frameIndex].buffer,
                                        static_cast<uint32_t>(_copyRegions.size()),
                                        _copyRegions.data());

                        VkMemoryBarrier transferBarrier{};
                        transferBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        transferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        transferBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT |
                                                 VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT |
                                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                             0, 1, &transferBarrier, 0, nullptr, 0, nullptr);
                    });
                }
            } else {
                ZoneScopedN("Upload All Chunks");
                _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                              _chunkDrawData.size() * sizeof(GPUChunkData));
            }

            for (uint32_t idx : _dirtyChunkList) {
                if (idx < _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices[idx] = false;
                }
            }
            _dirtyChunkList.clear();
        }

        _chunkDataDirty = false;
        _dirtyChunkCount = 0;
        _lastUploadTime = now;
    }
}

void ChunkBufferManager::uploadChunkData(uint32_t frameIndex) {
    ZoneScoped;
    _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                  _chunkDrawData.size() * sizeof(GPUChunkData));
}

void ChunkBufferManager::buildIndirectCommands() {
    ZoneScoped;

    _chunkDrawData.clear();
    _chunkDrawData.reserve(_chunkDrawInfos.size());

    for (const ChunkDrawInfo& drawInfo : _chunkDrawInfos) {
        const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;
        if (buffers.indexCount == 0) {
            continue;
        }

        GPUChunkData chunkData{};
        chunkData.chunkWorldPos = drawInfo.worldPosition;
        chunkData.indexCount = buffers.indexCount;
        chunkData.vertexBufferAddress = buffers.vertexAddress;
        chunkData.firstIndex = buffers.firstIndex;
        chunkData._padding = 0;
        _chunkDrawData.push_back(chunkData);
    }

    _indirectCommands.clear();
    _indirectCommands.reserve(_chunkDrawInfos.size());

    for (const auto& drawInfo : _chunkDrawInfos) {
        const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;
        if (buffers.indexCount == 0)
            continue;

        VkDrawIndexedIndirectCommand indirectCmd{};
        indirectCmd.indexCount = buffers.indexCount;
        indirectCmd.instanceCount = 1;
        indirectCmd.firstIndex = buffers.firstIndex;
        indirectCmd.vertexOffset = 0;
        indirectCmd.firstInstance = 0;
        _indirectCommands.push_back(indirectCmd);
    }
}

void ChunkBufferManager::rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks) {
    if (unloadedChunks.empty()) {
        return;
    }

    for (const glm::ivec3& pos : unloadedChunks) {
        auto it = _chunkDrawLookup.find(pos);
        if (it == _chunkDrawLookup.end()) {
            continue;
        }

        const size_t idx = it->second;
        ChunkDrawInfo& info = _chunkDrawInfos[idx];

        _meshPool->freeChunkBuffers(info.meshBuffers);

        info.meshBuffers.indexCount = 0;
        info.meshBuffers.vertexCount = 0;
        info.meshBuffers.vertexAddress = 0;
        info.meshBuffers.firstIndex = 0;
        info.meshBuffers.vertexBuffer.buffer = VK_NULL_HANDLE;
        info.meshBuffers.vertexBuffer.allocation = VK_NULL_HANDLE;

        _chunkDrawLookup.erase(it);
        _chunkDataDirty = true;
    }

    if (_chunkDrawInfos.empty()) {
        _meshPool->resetIndexOffset();
    }
}

float ChunkBufferManager::getMeshPoolUsage() const {
    if (!_meshPool) {
        return 0.0f;
    }
    const size_t totalIndexBytes = _meshPool->getTotalIndexMemory();
    const VkDeviceSize indexCapacity = _meshPool->getIndexBufferCapacity();

    if (indexCapacity == 0) {
        return 0.0f;
    }

    return std::min(1.0f, static_cast<float>(totalIndexBytes) / static_cast<float>(indexCapacity));
}

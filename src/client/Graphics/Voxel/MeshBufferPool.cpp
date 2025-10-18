#include "MeshBufferPool.hpp"

#include <stdexcept>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"

// Pre-allocate enough space for many chunks
// 256 million vertices and 512 million indices
constexpr VkDeviceSize VERTEX_BUFFER_SIZE = 256 * 1024 * 1024 * sizeof(uint32_t);
constexpr VkDeviceSize INDEX_BUFFER_SIZE = 512 * 1024 * 1024 * sizeof(uint32_t);

MeshBufferPool::MeshBufferPool(VulkanDevice& device, VulkanBuffer& bufferManager)
    : _device(device), _bufferManager(bufferManager) {

    _vertexBuffer = _bufferManager.createBuffer(
        VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    _indexBuffer = _bufferManager.createBuffer(
        INDEX_BUFFER_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
}

MeshBufferPool::~MeshBufferPool() {
    _bufferManager.destroyBuffer(_vertexBuffer);
    _bufferManager.destroyBuffer(_indexBuffer);
}

// Simple append-only allocator
// TODO: Implement a more sophisticated system to reclaim freed space
MeshAllocation MeshBufferPool::uploadMesh(
    std::span<uint32_t> indices, std::span<uint32_t> vertices,
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {
    const size_t vertexSize = vertices.size_bytes();
    const size_t indexSize = indices.size_bytes();

    // Check if there is enough space
    if ((_vertexOffset * sizeof(uint32_t)) + vertexSize > VERTEX_BUFFER_SIZE ||
        (_indexOffset * sizeof(uint32_t)) + indexSize > INDEX_BUFFER_SIZE) {
        throw std::runtime_error("MeshBufferPool is out of memory!");
    }

    MeshAllocation allocation;
    allocation.indexCount = static_cast<uint32_t>(indices.size());
    allocation.firstIndex = _indexOffset;
    allocation.vertexOffset = static_cast<int32_t>(_vertexOffset);

    // Calculate byte offsets in the mega-buffers
    const VkDeviceSize vertexByteOffset = _vertexOffset * sizeof(uint32_t);
    const VkDeviceSize indexByteOffset = _indexOffset * sizeof(uint32_t);

    // Create staging buffers for both vertex and index data (only if needed)
    AllocatedBuffer stagingVertex{VK_NULL_HANDLE, VK_NULL_HANDLE, {}};
    AllocatedBuffer stagingIndex{VK_NULL_HANDLE, VK_NULL_HANDLE, {}};

    if (!vertices.empty()) {
        stagingVertex = _bufferManager.createStagingBuffer(vertexSize);
        _bufferManager.uploadToBuffer(stagingVertex, vertices.data(), vertexSize);
    }

    if (!indices.empty()) {
        stagingIndex = _bufferManager.createStagingBuffer(indexSize);
        _bufferManager.uploadToBuffer(stagingIndex, indices.data(), indexSize);
    }

    // CRITICAL: Submit BOTH copies in a single command buffer to avoid multiple GPU stalls
    immediateSubmit([&](VkCommandBuffer cmd) {
        if (!vertices.empty()) {
            VkBufferCopy vertexCopy{};
            vertexCopy.srcOffset = 0;
            vertexCopy.dstOffset = vertexByteOffset;
            vertexCopy.size = vertexSize;
            vkCmdCopyBuffer(cmd, stagingVertex.buffer, _vertexBuffer.buffer, 1, &vertexCopy);
        }

        if (!indices.empty()) {
            VkBufferCopy indexCopy{};
            indexCopy.srcOffset = 0;
            indexCopy.dstOffset = indexByteOffset;
            indexCopy.size = indexSize;
            vkCmdCopyBuffer(cmd, stagingIndex.buffer, _indexBuffer.buffer, 1, &indexCopy);
        }
    });

    // Clean up staging buffers after the GPU transfer is complete
    if (stagingVertex.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(stagingVertex);
    }
    if (stagingIndex.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(stagingIndex);
    }

    // Update offsets for next allocation
    _vertexOffset += static_cast<uint32_t>(vertices.size());
    _indexOffset += static_cast<uint32_t>(indices.size());

    return allocation;
}

void MeshBufferPool::reset() {
    _vertexOffset = 0;
    _indexOffset = 0;
}

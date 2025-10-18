#include "MeshManager.hpp"

#include <cstring>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"

MeshManager::MeshManager(VulkanDevice& device, VulkanBuffer& bufferManager)
    : _device(device), _bufferManager(bufferManager) {}

// Overload for packed uint32_t voxel vertices
GPUMeshBuffers MeshManager::uploadMesh(
    std::span<uint32_t> indices, std::span<uint32_t> vertices,
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {
    const size_t vertexBufferSize = vertices.size() * sizeof(uint32_t);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface{};

    // Create vertex buffer
    newSurface.vertexBuffer = _bufferManager.createBuffer(
        vertexBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Find the address of the vertex buffer
    VkBufferDeviceAddressInfo deviceAddressInfo{.sType =
                                                    VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                .pNext = nullptr,
                                                .buffer = newSurface.vertexBuffer.buffer};
    newSurface.vertexBufferAddress =
        vkGetBufferDeviceAddress(_device.getDevice(), &deviceAddressInfo);

    // Create index buffer
    newSurface.indexBuffer = _bufferManager.createBuffer(
        indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Create staging buffer for both vertex and index data
    AllocatedBuffer staging =
        _bufferManager.createStagingBuffer(vertexBufferSize + indexBufferSize);

    void* data = staging.info.pMappedData;

    // Copy vertex buffer
    std::memcpy(data, vertices.data(), vertexBufferSize);
    // Copy index buffer
    std::memcpy(static_cast<char*>(data) + vertexBufferSize, indices.data(), indexBufferSize);

    // Upload to GPU using immediate submit
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{.srcOffset = 0, .dstOffset = 0, .size = vertexBufferSize};
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{
            .srcOffset = vertexBufferSize, .dstOffset = 0, .size = indexBufferSize};
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
    });

    _bufferManager.destroyBuffer(staging);

    return newSurface;
}

// Original overload for general Vertex struct
GPUMeshBuffers MeshManager::uploadMesh(
    std::span<uint32_t> indices, std::span<Vertex> vertices,
    const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit) {
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface{};

    // Create vertex buffer
    newSurface.vertexBuffer = _bufferManager.createBuffer(
        vertexBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Find the address of the vertex buffer
    VkBufferDeviceAddressInfo deviceAddressInfo{.sType =
                                                    VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                .pNext = nullptr,
                                                .buffer = newSurface.vertexBuffer.buffer};
    newSurface.vertexBufferAddress =
        vkGetBufferDeviceAddress(_device.getDevice(), &deviceAddressInfo);

    // Create index buffer
    newSurface.indexBuffer = _bufferManager.createBuffer(
        indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Create staging buffer for both vertex and index data
    AllocatedBuffer staging =
        _bufferManager.createStagingBuffer(vertexBufferSize + indexBufferSize);

    void* data = staging.info.pMappedData;

    // Copy vertex buffer
    std::memcpy(data, vertices.data(), vertexBufferSize);
    // Copy index buffer
    std::memcpy(static_cast<char*>(data) + vertexBufferSize, indices.data(), indexBufferSize);

    // Upload to GPU using immediate submit
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{.srcOffset = 0, .dstOffset = 0, .size = vertexBufferSize};
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{
            .srcOffset = vertexBufferSize, .dstOffset = 0, .size = indexBufferSize};
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
    });

    _bufferManager.destroyBuffer(staging);

    return newSurface;
}

void MeshManager::destroyMesh(const GPUMeshBuffers& mesh) {
    _bufferManager.destroyBuffer(mesh.vertexBuffer);
    _bufferManager.destroyBuffer(mesh.indexBuffer);
}

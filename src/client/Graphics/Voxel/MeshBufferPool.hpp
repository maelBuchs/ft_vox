#pragma once

#include <functional>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Core/VulkanTypes.hpp"

class VulkanDevice;
class VulkanBuffer;

// Represents a sub-allocation within a mega-buffer
struct MeshAllocation {
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
};

// Manages large buffers for storing all chunk meshes
class MeshBufferPool {
  public:
    MeshBufferPool(VulkanDevice& device, VulkanBuffer& bufferManager);
    ~MeshBufferPool();

    MeshBufferPool(const MeshBufferPool&) = delete;
    MeshBufferPool& operator=(const MeshBufferPool&) = delete;
    MeshBufferPool(MeshBufferPool&&) = delete;
    MeshBufferPool& operator=(MeshBufferPool&&) = delete;

    MeshAllocation
    uploadMesh(std::span<uint32_t> indices, std::span<uint32_t> vertices,
               const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit);
    void reset();

    [[nodiscard]] VkBuffer getVertexBuffer() const { return _vertexBuffer.buffer; }
    [[nodiscard]] VkBuffer getIndexBuffer() const { return _indexBuffer.buffer; }

  private:
    VulkanDevice& _device;
    VulkanBuffer& _bufferManager;

    AllocatedBuffer _vertexBuffer;
    AllocatedBuffer _indexBuffer;

    uint32_t _vertexOffset = 0;
    uint32_t _indexOffset = 0;
};

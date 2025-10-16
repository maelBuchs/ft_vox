#pragma once

#include <functional>
#include <span>

#include <vulkan/vulkan.h>

#include "VulkanTypes.hpp"

class VulkanDevice;
class VulkanBuffer;

class MeshManager {
  public:
    MeshManager(VulkanDevice& device, VulkanBuffer& bufferManager);
    ~MeshManager() = default;

    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;
    MeshManager(MeshManager&&) = delete;
    MeshManager& operator=(MeshManager&&) = delete;

    GPUMeshBuffers
    uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices,
               const std::function<void(std::function<void(VkCommandBuffer)>&&)>& immediateSubmit);
    void destroyMesh(const GPUMeshBuffers& mesh);

  private:
    VulkanDevice& _device;
    VulkanBuffer& _bufferManager;
};

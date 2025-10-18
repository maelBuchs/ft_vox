#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "../Core/VulkanTypes.hpp"
#include "../Pipeline/Pipeline.hpp"
#include "common/Types/RenderTypes.hpp"
#include "MeshBufferPool.hpp"


class VulkanDevice;
class MeshManager;
class BlockRegistry;
class Chunk;
class Camera;
class RenderContext;
class CommandExecutor;
class MeshBufferPool;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
struct MeshAllocation;

class VoxelRenderer {
  public:
    VoxelRenderer(VulkanDevice& device, MeshManager& meshManager, BlockRegistry& registry,
                  RenderContext& context, CommandExecutor& executor, VulkanBuffer& bufferManager,
                  DescriptorAllocatorGrowable& descriptorAllocator);
    ~VoxelRenderer();

    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;
    VoxelRenderer(VoxelRenderer&&) = delete;
    VoxelRenderer& operator=(VoxelRenderer&&) = delete;

    void initPipelines();
    void initTestChunk();
    void drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode);

  private:
    void initMDI();

    VulkanDevice& _device;
    MeshManager& _meshManager;
    BlockRegistry& _blockRegistry;
    RenderContext& _context;
    CommandExecutor& _executor;
    VulkanBuffer& _bufferManager;
    DescriptorAllocatorGrowable& _descriptorAllocator;

    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;

    std::unique_ptr<Chunk> _testChunk;

    // --- MDI Resources ---
    std::unique_ptr<MeshBufferPool> _meshPool;

    // This mesh data will be shared by all chunk instances
    MeshAllocation _sharedChunkMeshAllocation;

    // A list of world positions for each chunk instance we want to draw
    std::vector<glm::vec3> _chunkPositions;

    AllocatedBuffer _indirectBuffer;
    AllocatedBuffer _chunkDataBuffer;

    std::vector<VkDrawIndexedIndirectCommand> _indirectCommands;
    std::vector<GPUChunkData> _chunkDrawData;

    // Descriptor set for chunk data SSBO
    VkDescriptorSetLayout _chunkSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _chunkDescriptorSet = VK_NULL_HANDLE;
};

#pragma once

#include <memory>

#include <vulkan/vulkan.h>

#include "Pipeline.hpp"
#include "VulkanTypes.hpp"

class VulkanDevice;
class MeshManager;
class BlockRegistry;
class Chunk;
class Camera;
class RenderContext;
class CommandExecutor;

class VoxelRenderer {
  public:
    VoxelRenderer(VulkanDevice& device, MeshManager& meshManager, BlockRegistry& registry,
                  RenderContext& context, CommandExecutor& executor);
    ~VoxelRenderer();

    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;
    VoxelRenderer(VoxelRenderer&&) = delete;
    VoxelRenderer& operator=(VoxelRenderer&&) = delete;

    void initPipelines();
    void initTestChunk();
    void drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode);

  private:
    VulkanDevice& _device;
    MeshManager& _meshManager;
    BlockRegistry& _blockRegistry;
    RenderContext& _context;
    CommandExecutor& _executor;

    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;
    GPUMeshBuffers _testChunkMesh;
    std::unique_ptr<Chunk> _testChunk;
};

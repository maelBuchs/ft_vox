#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "../Core/VulkanTypes.hpp"
#include "../Pipeline/Pipeline.hpp"
#include "common/Protocol/Protocol.hpp"
#include "common/Types/RenderTypes.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "MeshBufferPool.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

class VulkanDevice;
class MeshManager;
class BlockRegistry;
class Camera;
class RenderContext;
class CommandExecutor;
class MeshBufferPool;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
class Renderer;
struct MeshAllocation;

class VoxelRenderer {
  public:
    VoxelRenderer(VulkanDevice& device, MeshManager& meshManager, BlockRegistry& registry,
                  RenderContext& context, CommandExecutor& executor, VulkanBuffer& bufferManager,
                  DescriptorAllocatorGrowable& descriptorAllocator, Renderer& renderer,
                  ThreadSafeQueue<MeshData>& finishedMeshQueue);
    ~VoxelRenderer();

    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;
    VoxelRenderer(VoxelRenderer&&) = delete;
    VoxelRenderer& operator=(VoxelRenderer&&) = delete;

    void initPipelines(VkImageView atlasView, VkSampler atlasSampler);
    void initTestChunk();

    /**
     * Update: Check for finished meshes and upload to GPU.
     * Call this every frame BEFORE drawVoxels.
     */
    void update();

    void drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode);

  private:
    void initMDI(VkImageView atlasView, VkSampler atlasSampler);

    VulkanDevice& _device;
    MeshManager& _meshManager;
    BlockRegistry& _blockRegistry;
    RenderContext& _context;
    CommandExecutor& _executor;
    VulkanBuffer& _bufferManager;
    DescriptorAllocatorGrowable& _descriptorAllocator;
    Renderer& _renderer;

    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;

    VkPipelineLayout _voxelPipelineLayout = VK_NULL_HANDLE;

    std::unique_ptr<Chunk> _testChunk;

    // --- MDI Resources ---
    std::unique_ptr<MeshBufferPool> _meshPool;

    struct ChunkDrawInfo {
        glm::ivec3 chunkCoords{};
        glm::vec3 worldPosition{};
        MeshAllocation mesh{};
    };

    std::vector<ChunkDrawInfo> _chunkDrawInfos;
    std::unordered_map<glm::ivec3, size_t> _chunkDrawLookup;

    // Queue for receiving finished mesh data from meshing threads
    ThreadSafeQueue<MeshData>& _finishedMeshQueue;

    AllocatedBuffer _indirectBuffer;
    AllocatedBuffer _chunkDataBuffer;

    std::vector<VkDrawIndexedIndirectCommand> _indirectCommands;
    std::vector<GPUChunkData> _chunkDrawData;

    // Descriptor set for chunk data SSBO
    VkDescriptorSetLayout _chunkSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet _chunkDescriptorSet = VK_NULL_HANDLE;
};

#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

// Forward declarations
struct MeshData;
template<typename T> class ThreadSafeQueue;

class VulkanDevice;
class MeshManager;
class BlockRegistry;
class Camera;
class RenderContext;
class CommandExecutor;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
class Renderer;
class VoxelPipelineManager;
class ChunkBufferManager;
class VoxelDrawDispatcher;


/**
 * @brief Facade for voxel rendering system.
 *
 * This class provides a simplified interface to the voxel rendering subsystem,
 * delegating to specialized managers:
 * - VoxelPipelineManager: Pipeline and descriptor layout management
 * - ChunkBufferManager: GPU buffers and chunk data management
 * - VoxelDrawDispatcher: Draw command recording and dispatch
 */
class VoxelRenderer {
  public:
    VoxelRenderer(VulkanDevice& device, MeshManager& meshManager, BlockRegistry& registry,
                  RenderContext& context, CommandExecutor& executor, VulkanBuffer& bufferManager,
                  DescriptorAllocatorGrowable& descriptorAllocator, Renderer& renderer,
                  std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues);
    ~VoxelRenderer();

    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;
    VoxelRenderer(VoxelRenderer&&) = delete;
    VoxelRenderer& operator=(VoxelRenderer&&) = delete;

    /**
     * @brief Initialize all pipelines and resources.
     * @param atlasView Texture atlas image view
     * @param atlasSampler Texture atlas sampler
     * @param texturesPerRow Number of textures per row in atlas
     */
    void initPipelines(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow);

    /**
     * @brief Update chunk data from mesh queues.
     * Call this every frame BEFORE drawVoxels.
     * @param cameraChunkPos Current camera chunk position for distance checks
     * @param maxLoadDistance Maximum distance (in chunks) to accept new meshes
     */
    void update(const glm::ivec3& cameraChunkPos, int maxLoadDistance);

    /**
     * @brief Record voxel draw commands.
     * @param cmd Command buffer to record to
     * @param camera Camera for view/projection
     * @param wireframeMode Whether to use wireframe rendering
     */
    void drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode);

    /**
     * @brief Rebuild mesh pool for unloaded chunks.
     * @param unloadedChunks List of chunk positions that were unloaded
     */
    void rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks);

    /**
     * @brief Get the current number of chunks loaded in the renderer.
     */
    [[nodiscard]] size_t getLoadedChunkCount() const;

    /**
     * @brief Get mesh buffer pool usage (0.0 to 1.0).
     */
    [[nodiscard]] float getMeshPoolUsage() const;
    [[nodiscard]] uint32_t getLodResidentCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodSubmittedCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodMeshUpdateCount(uint32_t lodLevel) const;
    [[nodiscard]] uint32_t getLodUpgradeCount() const;
    [[nodiscard]] uint32_t getLodDowngradeCount() const;

  private:
    VulkanDevice& _device;
    RenderContext& _context;
    Renderer& _renderer;

    // Delegated managers
    std::unique_ptr<VoxelPipelineManager> _pipelineManager;
    std::unique_ptr<ChunkBufferManager> _bufferManager;
    std::unique_ptr<VoxelDrawDispatcher> _drawDispatcher;

    // GPU culling flag (currently disabled - task shader does culling)
    bool _enableGPUCulling = false;
};

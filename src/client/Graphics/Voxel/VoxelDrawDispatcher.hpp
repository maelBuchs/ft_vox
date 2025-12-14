#pragma once

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "../Core/VulkanTypes.hpp"
#include "common/Types/RenderTypes.hpp"

class VulkanDevice;
class VulkanBuffer;
class RenderContext;
class Renderer;
class Camera;
class VoxelPipelineManager;
class ChunkBufferManager;

/**
 * @brief Handles voxel draw command recording and GPU dispatch.
 *
 * Responsible for:
 * - Camera data upload
 * - GPU frustum culling dispatch
 * - Rendering attachment setup
 * - Mesh shader / Traditional pipeline draw dispatch
 */
class VoxelDrawDispatcher {
  public:
    VoxelDrawDispatcher(VulkanDevice& device, VulkanBuffer& bufferManager,
                        RenderContext& context, Renderer& renderer,
                        VoxelPipelineManager& pipelineManager,
                        ChunkBufferManager& bufferMgr);
    ~VoxelDrawDispatcher() = default;

    VoxelDrawDispatcher(const VoxelDrawDispatcher&) = delete;
    VoxelDrawDispatcher& operator=(const VoxelDrawDispatcher&) = delete;
    VoxelDrawDispatcher(VoxelDrawDispatcher&&) = delete;
    VoxelDrawDispatcher& operator=(VoxelDrawDispatcher&&) = delete;

    /**
     * @brief Record voxel draw commands.
     * @param cmd Command buffer to record to
     * @param camera Camera for view/projection
     * @param wireframeMode Whether to use wireframe rendering
     * @param enableGPUCulling Whether GPU frustum culling is enabled
     */
    void draw(VkCommandBuffer cmd, Camera& camera, bool wireframeMode, bool enableGPUCulling);

  private:
    void uploadCameraData(VkCommandBuffer cmd, Camera& camera, uint32_t frameIndex);
    void dispatchGPUCulling(VkCommandBuffer cmd, Camera& camera, int maxLoadDistance);
    void buildCPUIndirectCommands(VkCommandBuffer cmd);
    void drawMeshShaderPath(VkCommandBuffer cmd, bool wireframeMode);
    void drawTraditionalPath(VkCommandBuffer cmd, Camera& camera, bool wireframeMode, bool enableGPUCulling);

    VulkanDevice& _device;
    VulkanBuffer& _bufferManager;
    RenderContext& _context;
    Renderer& _renderer;
    VoxelPipelineManager& _pipelineManager;
    ChunkBufferManager& _bufferMgr;
};


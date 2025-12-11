#pragma once

#include <array>

#include <vulkan/vulkan.h>

#include "../Core/VulkanTypes.hpp"
#include "../Pipeline/Pipeline.hpp"

class VulkanDevice;
class VulkanBuffer;
class DescriptorAllocatorGrowable;
class RenderContext;
class MeshBufferPool;

/**
 * @brief Manages Vulkan pipelines and descriptor layouts for voxel rendering.
 *
 * Handles creation, configuration, and destruction of:
 * - Traditional vertex/fragment pipelines (filled + wireframe)
 * - Mesh shader pipelines (task/mesh/fragment - filled + wireframe)
 * - Compute frustum culling pipeline
 * - All descriptor set layouts
 */
class VoxelPipelineManager {
  public:
    VoxelPipelineManager(VulkanDevice& device, RenderContext& context,
                         DescriptorAllocatorGrowable& descriptorAllocator);
    ~VoxelPipelineManager();

    VoxelPipelineManager(const VoxelPipelineManager&) = delete;
    VoxelPipelineManager& operator=(const VoxelPipelineManager&) = delete;
    VoxelPipelineManager(VoxelPipelineManager&&) = delete;
    VoxelPipelineManager& operator=(VoxelPipelineManager&&) = delete;

    // --- Initialization ---

    /**
     * @brief Initialize traditional MDI descriptor layouts.
     * Must be called first before other init methods.
     */
    void initDescriptorLayouts();

    /**
     * @brief Initialize traditional vertex/fragment pipelines.
     * Requires descriptor layouts to be initialized.
     */
    void initTraditionalPipelines();

	/**
	 * @brief Initialize compute frustum culling pipeline.
	 */
    void initComputeCullingPipeline();

    /**
     * @brief Initialize mesh shader pipelines (task/mesh/fragment).
     * @return true if mesh shaders are available and initialized
     */
    bool initMeshShaderPipelines();

    // --- Accessors ---

    // Pipelines
    [[nodiscard]] Pipeline& getVoxelPipeline() { return _voxelPipeline; }
    [[nodiscard]] Pipeline& getVoxelWireframePipeline() { return _voxelWireframePipeline; }
    [[nodiscard]] Pipeline& getMeshShaderPipeline() { return _meshShaderPipeline; }
    [[nodiscard]] Pipeline& getMeshShaderWireframePipeline() { return _meshShaderWireframePipeline; }
    [[nodiscard]] Pipeline& getFrustumCullPipeline() { return _frustumCullPipeline; }

    // Pipeline Layouts
    [[nodiscard]] VkPipelineLayout getVoxelPipelineLayout() const { return _voxelPipelineLayout; }
    [[nodiscard]] VkPipelineLayout getMeshShaderPipelineLayout() const { return _meshShaderPipelineLayout; }
    [[nodiscard]] VkPipelineLayout getFrustumCullPipelineLayout() const { return _frustumCullPipelineLayout; }

    // Descriptor Set Layouts
    [[nodiscard]] VkDescriptorSetLayout getChunkSetLayout() const { return _chunkSetLayout; }
    [[nodiscard]] VkDescriptorSetLayout getTraditionalFragSetLayout() const { return _traditionalFragSetLayout; }
    [[nodiscard]] VkDescriptorSetLayout getMeshShaderSetLayout() const { return _meshShaderSetLayout; }
    [[nodiscard]] VkDescriptorSetLayout getMeshShaderFragSetLayout() const { return _meshShaderFragSetLayout; }
    [[nodiscard]] VkDescriptorSetLayout getFrustumCullSetLayout() const { return _frustumCullSetLayout; }

    // Mesh Shader Support
    [[nodiscard]] bool supportsMeshShaders() const { return _useMeshShaders; }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksEXT getDrawMeshTasksFunc() const { return _vkCmdDrawMeshTasksEXT; }
    [[nodiscard]] uint32_t getMaxMeshWorkgroupsPerTask() const { return _maxMeshWorkgroupsPerTask; }

    // Compute Support
    [[nodiscard]] bool supportsDrawIndirectCount() const { return _supportsDrawIndirectCount; }

  private:
    VulkanDevice& _device;
    RenderContext& _context;
    DescriptorAllocatorGrowable& _descriptorAllocator;

    // --- Pipelines ---
    Pipeline _voxelPipeline;
    Pipeline _voxelWireframePipeline;
    Pipeline _meshShaderPipeline;
    Pipeline _meshShaderWireframePipeline;
    Pipeline _frustumCullPipeline;

    // --- Pipeline Layouts ---
    VkPipelineLayout _voxelPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout _meshShaderPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout _frustumCullPipelineLayout = VK_NULL_HANDLE;

    // --- Descriptor Set Layouts ---
    VkDescriptorSetLayout _chunkSetLayout = VK_NULL_HANDLE;           // Set 0: Chunk SSBO + atlas
    VkDescriptorSetLayout _traditionalFragSetLayout = VK_NULL_HANDLE; // Set 1: Fragment atlas
    VkDescriptorSetLayout _meshShaderSetLayout = VK_NULL_HANDLE;      // Set 0: Camera, chunks, indices
    VkDescriptorSetLayout _meshShaderFragSetLayout = VK_NULL_HANDLE;  // Set 1: Fragment atlas
    VkDescriptorSetLayout _frustumCullSetLayout = VK_NULL_HANDLE;     // Compute culling layout

    // --- Mesh Shader State ---
    bool _useMeshShaders = false;
    PFN_vkCmdDrawMeshTasksEXT _vkCmdDrawMeshTasksEXT = nullptr;
    uint32_t _maxMeshWorkgroupsPerTask = 1;

    // --- Compute State ---
    bool _supportsDrawIndirectCount = false;
};

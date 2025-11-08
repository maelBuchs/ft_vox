#include "VoxelRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <tracy/Tracy.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../../Game/Camera.hpp"
#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
#include "../Pipeline/ComputePipelineBuilder.hpp"
#include "../Pipeline/GraphicsPipelineBuilder.hpp"
#include "../Renderer.hpp"
#include "../Rendering/CommandExecutor.hpp"
#include "../Rendering/RenderContext.hpp"
#include "common/World/Chunk.hpp"
#include "common/World/ChunkMesh.hpp"
#include "MeshBufferPool.hpp"
#include "MeshManager.hpp"

VoxelRenderer::VoxelRenderer(VulkanDevice& device, MeshManager& meshManager,
                             BlockRegistry& registry, RenderContext& context,
                             CommandExecutor& executor, VulkanBuffer& bufferManager,
                             DescriptorAllocatorGrowable& descriptorAllocator, Renderer& renderer,
                             std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues)
    : _device(device), _meshManager(meshManager), _blockRegistry(registry), _context(context),
      _executor(executor), _bufferManager(bufferManager), _descriptorAllocator(descriptorAllocator),
      _renderer(renderer), _perThreadMeshQueues(perThreadMeshQueues) {
    // Initialize mesh buffer pool
    _meshPool = std::make_unique<MeshBufferPool>(_device, _bufferManager);
}

VoxelRenderer::~VoxelRenderer() {
    auto startTime = std::chrono::high_resolution_clock::now();
    std::cout << "[VoxelRenderer] Destructor start - " << _chunkDrawInfos.size()
              << " chunks loaded\n";

    // CRITICAL: Free all chunk vertex buffers BEFORE clearing vectors!
    // VMA requires explicit deallocation before the allocator is destroyed.

    // BUGFIX: Flush deletion queue FIRST to avoid double-free
    // This destroys any buffers that were queued for deletion but not yet processed
    if (_meshPool) {
        _meshPool->flushDeletionQueue();
    }

    // Extract mesh buffers into temp vector for batch destruction (fast!)
    // Only include buffers that are actually allocated (skip empty chunks)
    std::vector<ChunkMeshBuffers> allBuffers;
    allBuffers.reserve(_chunkDrawInfos.size());
    for (const auto& info : _chunkDrawInfos) {
        // Skip empty chunks (indexCount=0 means buffer was never allocated or already freed)
        if (info.meshBuffers.vertexBuffer.buffer != VK_NULL_HANDLE &&
            info.meshBuffers.indexCount > 0) {
            allBuffers.push_back(info.meshBuffers);
        }
    }

    // Batch destroy all remaining vertex buffers (fast ~70ms for 15k chunks)
    if (!allBuffers.empty() && _meshPool) {
        _meshPool->destroyAllChunkBuffers(allBuffers);
    }

    // Now safe to clear vectors (no leaks!)
    _chunkDrawInfos.clear();
    _chunkDrawLookup.clear();

    std::cout << "[VoxelRenderer] Cleanup took: "
              << std::chrono::duration<double, std::milli>(
                     std::chrono::high_resolution_clock::now() - startTime)
                     .count()
              << "ms\n";

    // CRITICAL: Wait for GPU to finish before destroying resources
    // This prevents "pipeline layout in use" validation errors
    // Even though FrameManager waits for fences, we add a safety check here
    vkDeviceWaitIdle(_device.getDevice());

    _voxelPipeline.cleanup(_device);
    _voxelWireframePipeline.cleanup(_device);
    _meshShaderPipeline.cleanup(_device);
    _meshShaderWireframePipeline.cleanup(_device);

    // Clean up compute culling pipeline
    _frustumCullPipeline.cleanup(_device);

    // CRITICAL: Wait again after pipeline destruction to ensure they're fully destroyed
    // before destroying the layouts they reference. This prevents the layout leak.
    vkDeviceWaitIdle(_device.getDevice());

    // Clean up owned pipeline layouts
    if (_voxelPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _voxelPipelineLayout, nullptr);
        _voxelPipelineLayout = VK_NULL_HANDLE;
    }
    if (_meshShaderPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _meshShaderPipelineLayout, nullptr);
        _meshShaderPipelineLayout = VK_NULL_HANDLE;
    }
    if (_frustumCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _frustumCullPipelineLayout, nullptr);
        _frustumCullPipelineLayout = VK_NULL_HANDLE;
    }

    // Clean up descriptor set layouts
    if (_chunkSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _chunkSetLayout, nullptr);
    }
    if (_meshShaderSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _meshShaderSetLayout, nullptr);
    }
    if (_meshShaderFragSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _meshShaderFragSetLayout, nullptr);
    }
    if (_frustumCullSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _frustumCullSetLayout, nullptr);
    }

    // Clean up MDI buffers
    if (_indirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indirectBuffer);
    }
    // PHASE 1: Clean up double-buffered chunk data buffers
    for (auto& buffer : _chunkDataBuffers) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            _bufferManager.destroyBuffer(buffer);
        }
    }
    if (_atlasConfigBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_atlasConfigBuffer);
    }
    if (_cameraUniformBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_cameraUniformBuffer);
    }

    // Clean up compute culling buffers
    if (_frustumUniformBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_frustumUniformBuffer);
    }
    if (_culledIndirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_culledIndirectBuffer);
    }
    if (_culledChunkDataBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_culledChunkDataBuffer);
    }
}

void VoxelRenderer::initPipelines(VkImageView atlasView, VkSampler atlasSampler,
                                  int texturesPerRow) {
    // First initialize MDI resources and descriptor set layout
    initMDI(atlasView, atlasSampler, texturesPerRow);

    // Initialize GPU frustum culling compute pipeline
    initComputeCulling();

    // Initialize mesh shader pipeline if supported
    initMeshShaderPipeline(atlasView, atlasSampler);

    // Update culled chunk descriptor set with atlas bindings (now that they exist)
    DescriptorWriter culledWriter;
    culledWriter.writeBuffer(0, _culledChunkDataBuffer.buffer,
                             sizeof(GPUChunkData) * _currentMaxChunks, 0,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    culledWriter.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    culledWriter.writeBuffer(2, _atlasConfigBuffer.buffer, sizeof(int), 0,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    culledWriter.updateSet(_device.getDevice(), _culledChunkDescriptorSet);

    // ===== Traditional Vertex/Fragment Pipeline (fallback for GPUs without mesh shader support)
    // ===== Skip if mesh shaders are available - they use a different fragment shader layout
    if (!_useMeshShaders) {
        VkShaderModule voxelFragShader =
            Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");
        VkShaderModule voxelVertexShader =
            Pipeline::loadShaderModule(_device, "shaders/voxel.vert.spv");

        VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(glm::mat4)};

        // Update pipeline layout to include descriptor set for chunk data SSBO
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &_chunkSetLayout, // Include descriptor set for SSBO
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange};

        // Create and store the shared pipeline layout
        if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutInfo, nullptr,
                                   &_voxelPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create voxel pipeline layout");
        }

        // ========================================================================
        // NO VERTEX INPUT BINDINGS - We use buffer device address!
        // ========================================================================
        // Vertices are accessed via buffer_reference in the shader
        // No traditional vertex input needed
        std::vector<VkVertexInputBindingDescription> noBindings{};
        std::vector<VkVertexInputAttributeDescription> noAttributes{};

        const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
        const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();

        // Create FILLED pipeline
        GraphicsPipelineBuilder pipelineBuilder;
        pipelineBuilder.setPipelineLayout(_voxelPipelineLayout);
        pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
        pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
        pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();
        pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
        pipelineBuilder.setColorAttachmentFormat(drawImage.format);
        pipelineBuilder.setDepthFormat(depthImage.format);
        pipelineBuilder.setVertexInputState(noBindings, noAttributes); // No vertex input!

        VkPipeline voxelPipeline = pipelineBuilder.build(_device.getDevice());
        _voxelPipeline.init(voxelPipeline, _voxelPipelineLayout);

        // Create WIREFRAME pipeline
        pipelineBuilder.clear();
        pipelineBuilder.setPipelineLayout(_voxelPipelineLayout);
        pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
        pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_LINE); // WIREFRAME MODE
        pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();
        pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
        pipelineBuilder.setColorAttachmentFormat(drawImage.format);
        pipelineBuilder.setDepthFormat(depthImage.format);
        pipelineBuilder.setVertexInputState(noBindings, noAttributes); // No vertex input!

        VkPipeline voxelWireframePipeline = pipelineBuilder.build(_device.getDevice());
        _voxelWireframePipeline.init(voxelWireframePipeline, _voxelPipelineLayout);

        vkDestroyShaderModule(_device.getDevice(), voxelFragShader, nullptr);
        vkDestroyShaderModule(_device.getDevice(), voxelVertexShader, nullptr);
    } // End of traditional pipeline creation (only for non-mesh-shader GPUs)

    // PHASE 1: Update mesh shader index buffer binding for all frame descriptor sets
    if (_useMeshShaders && _meshPool) {
        VkBuffer indexBuffer = _meshPool->getIndexBuffer();
        VkDeviceSize indexBufferSize = _meshPool->getIndexBufferCapacity();

        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter indexWriter;
            indexWriter.writeBuffer(2, indexBuffer, indexBufferSize, 0,
                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            indexWriter.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }

        std::cout << "[VoxelRenderer] Mesh shader index buffer binding updated for all frames\n";
    }
}

void VoxelRenderer::initMDI(VkImageView atlasView, VkSampler atlasSampler, int texturesPerRow) {
    // Create descriptor set layout for chunk data SSBO, texture atlas, and atlas config
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    layoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // Atlas configuration
    _chunkSetLayout = layoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_VERTEX_BIT |
                                                                   VK_SHADER_STAGE_FRAGMENT_BIT);

    // Initialize with default capacity
    _currentMaxChunks = MAX_CHUNKS;

    // Create buffers for indirect draw commands
    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // PHASE 1: Create double-buffered chunk data SSBOs (eliminates vkDeviceWaitIdle)
    // Each frame uses its own buffer to avoid GPU-CPU race conditions
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDataBuffers[i] = _bufferManager.createBuffer(sizeof(GPUChunkData) * _currentMaxChunks,
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // Create uniform buffer for atlas configuration
    _atlasConfigBuffer = _bufferManager.createBuffer(
        sizeof(int), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Upload texturesPerRow to the uniform buffer
    _bufferManager.uploadToBuffer(_atlasConfigBuffer, &texturesPerRow, sizeof(int));

    // PHASE 1: Allocate per-frame descriptor sets for double-buffered chunk data
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDescriptorSets[i] =
            _descriptorAllocator.allocate(_device.getDevice(), _chunkSetLayout, nullptr);

        // Write descriptor set to bind the chunk data buffer, texture atlas, and atlas config
        DescriptorWriter writer;
        writer.writeBuffer(0, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * _currentMaxChunks, 0,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.writeBuffer(2, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        writer.updateSet(_device.getDevice(), _chunkDescriptorSets[i]);
    }
}

void VoxelRenderer::initComputeCulling() {
    ZoneScoped; // Tracy profiling

    std::cout << "[VoxelRenderer] Initializing GPU frustum culling compute pipeline...\n";

    // ===== 1. Create descriptor set layout for compute shader =====
    // Binding 0: Input chunk data (SSBO, read-only)
    // Binding 1: Frustum uniform data (UBO)
    // Binding 2: Output indirect commands (SSBO, write-only)
    // Binding 3: Output compacted chunk data (SSBO, write-only)

    DescriptorLayoutBuilder computeLayoutBuilder;
    computeLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Input chunks
    computeLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // Frustum data
    computeLayoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Output indirect
    computeLayoutBuilder.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Output chunk data

    _frustumCullSetLayout =
        computeLayoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_COMPUTE_BIT);

    // ===== 2. Create push constant range =====
    VkPushConstantRange computePushRange{};
    computePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computePushRange.offset = 0;
    computePushRange.size = sizeof(ComputePushConstants);

    // ===== 3. Build compute pipeline (builder creates the pipeline layout for us) =====
    ComputePipelineBuilder computeBuilder;
    computeBuilder.setShader("shaders/frustum_cull.comp.spv");
    computeBuilder.setDescriptorSetLayout(_frustumCullSetLayout);
    computeBuilder.setPushConstantRange(computePushRange);

    ComputePipelineBuilder::BuildResult buildResult = computeBuilder.build(_device);

    // Store the builder's pipeline layout (no need to create it manually)
    _frustumCullPipelineLayout = buildResult.layout;
    _frustumCullPipeline.init(buildResult.pipeline, _frustumCullPipelineLayout);

    // ===== 5. Create buffers =====
    // Frustum uniform buffer (updated per frame with camera frustum)
    _frustumUniformBuffer = _bufferManager.createBuffer(sizeof(GPUFrustumData),
                                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Output indirect commands buffer (compute writes, graphics reads)
    // Format: uint32_t drawCount + VkDrawIndexedIndirectCommand[]
    const size_t indirectBufferSize =
        sizeof(uint32_t) + (sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks);
    _culledIndirectBuffer = _bufferManager.createBuffer(indirectBufferSize,
                                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VMA_MEMORY_USAGE_GPU_ONLY);

    // Output compacted chunk data buffer
    _culledChunkDataBuffer = _bufferManager.createBuffer(sizeof(GPUChunkData) * _currentMaxChunks,
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_GPU_ONLY);

    // ===== 6. Allocate and write descriptor set =====
    _frustumCullDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _frustumCullSetLayout, nullptr);

    DescriptorWriter computeWriter;
    // Binding 0: Input chunk data (use frame 0's buffer for compute - compute path is disabled anyway)
    computeWriter.writeBuffer(0, _chunkDataBuffers[0].buffer, sizeof(GPUChunkData) * _currentMaxChunks,
                              0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Binding 1: Frustum uniform data
    computeWriter.writeBuffer(1, _frustumUniformBuffer.buffer, sizeof(GPUFrustumData), 0,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    // Binding 2: Output indirect commands
    computeWriter.writeBuffer(2, _culledIndirectBuffer.buffer, indirectBufferSize, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Binding 3: Output compacted chunk data
    computeWriter.writeBuffer(3, _culledChunkDataBuffer.buffer,
                              sizeof(GPUChunkData) * _currentMaxChunks, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    computeWriter.updateSet(_device.getDevice(), _frustumCullDescriptorSet);

    // ===== 6.5. Create descriptor set for graphics pipeline to read culled chunk data =====
    _culledChunkDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _chunkSetLayout, nullptr);

    // Write descriptor set for culled chunk data (graphics will use this after compute culling)
    DescriptorWriter culledWriter;
    culledWriter.writeBuffer(0, _culledChunkDataBuffer.buffer,
                             sizeof(GPUChunkData) * _currentMaxChunks, 0,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    // Bindings 1 and 2 (texture atlas and config) are the same, need to copy from
    // _chunkDescriptorSets[0] For now, we'll update them separately - TODO: optimize by reusing
    culledWriter.updateSet(_device.getDevice(), _culledChunkDescriptorSet);

    // ===== 7. Check if drawIndirectCount feature is supported =====
    VkPhysicalDeviceVulkan12Features supportedFeatures12{};
    supportedFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &supportedFeatures12;

    vkGetPhysicalDeviceFeatures2(_device.getPhysicalDevice(), &features2);

    _supportsDrawIndirectCount = (supportedFeatures12.drawIndirectCount == VK_TRUE);

    if (_supportsDrawIndirectCount) {
        std::cout << "[VoxelRenderer] drawIndirectCount feature: SUPPORTED\n";
    } else {
        std::cout << "[VoxelRenderer] WARNING: drawIndirectCount NOT supported - using fallback\n";
    }

    std::cout << "[VoxelRenderer] GPU frustum culling initialized successfully!\n";
}

void VoxelRenderer::initMeshShaderPipeline(VkImageView atlasView, VkSampler atlasSampler) {
    ZoneScoped;

    // Initialize camera UBO to null first to prevent corruption
    _cameraUniformBuffer.buffer = VK_NULL_HANDLE;
    _cameraUniformBuffer.allocation = VK_NULL_HANDLE;

    // Check if mesh shaders are supported
    if (!_device.supportsMeshShaders()) {
        std::cout << "[VoxelRenderer] Mesh shaders not supported - skipping mesh shader pipeline\n";
        _useMeshShaders = false;
        return;
    }

    std::cout << "[VoxelRenderer] Initializing task/mesh shader pipeline...\n";
    _useMeshShaders = true;

    // Load vkCmdDrawMeshTasksEXT function pointer
    _vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        vkGetDeviceProcAddr(_device.getDevice(), "vkCmdDrawMeshTasksEXT"));

    if (_vkCmdDrawMeshTasksEXT == nullptr) {
        std::cout << "[VoxelRenderer] ERROR: Failed to load vkCmdDrawMeshTasksEXT - extension may "
                     "not be enabled\n";
        std::cout << "[VoxelRenderer] Falling back to traditional rendering\n";
        _useMeshShaders = false;
        return;
    }

    std::cout << "[VoxelRenderer] Successfully loaded vkCmdDrawMeshTasksEXT function pointer\n";

    VkPhysicalDeviceMeshShaderPropertiesEXT meshProps{};
    meshProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    meshProps.pNext = nullptr;

    VkPhysicalDeviceProperties2 deviceProps{};
    deviceProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProps.pNext = &meshProps;
    vkGetPhysicalDeviceProperties2(_device.getPhysicalDevice(), &deviceProps);

    _maxMeshWorkgroupsPerTask = std::max(meshProps.maxMeshWorkGroupCount[0], 1u);
    std::cout << "[VoxelRenderer] Device supports " << _maxMeshWorkgroupsPerTask
              << " mesh workgroups per task workgroup\n";

    // ===== 1. Create descriptor set layouts =====
    // Set 0: Mesh/task shader data (camera, chunk data, index buffer)
    // Set 1: Fragment shader data (texture atlas) - create new layout

    DescriptorLayoutBuilder meshLayoutBuilder;
    meshLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // Camera UBO
    meshLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Chunk data
    meshLayoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Index buffer

    _meshShaderSetLayout = meshLayoutBuilder.build(
        _device.getDevice(), VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT);

    // Create descriptor layout for fragment shader (texture atlas only)
    DescriptorLayoutBuilder fragLayoutBuilder;
    fragLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // Texture atlas
    fragLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);         // Atlas config

    _meshShaderFragSetLayout =
        fragLayoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_FRAGMENT_BIT);

    // ===== 2. Create camera UBO =====
    // Note: _cameraUniformBuffer is already initialized to null at the start of this function
    _cameraUniformBuffer = _bufferManager.createBuffer(sizeof(GPUCameraData),
                                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_CPU_TO_GPU);

    // ===== 3. Create push constant range for mesh shader =====
    struct MeshShaderPushConstants {
        uint32_t totalChunks;
        float chunkSize;
        uint32_t maxVerticesPerMeshWorkgroup;
        uint32_t maxMeshWorkgroupsPerTask;
    };

    VkPushConstantRange meshPushRange{};
    meshPushRange.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT;
    meshPushRange.offset = 0;
    meshPushRange.size = sizeof(MeshShaderPushConstants);

    // ===== 4. Create pipeline layout =====
    // Use both descriptor sets: set 0 for mesh data, set 1 for fragment shader (texture atlas)
    VkDescriptorSetLayout setLayouts[] = {_meshShaderSetLayout, _meshShaderFragSetLayout};

    VkPipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshLayoutInfo.setLayoutCount = 2; // Two descriptor sets
    meshLayoutInfo.pSetLayouts = setLayouts;
    meshLayoutInfo.pushConstantRangeCount = 1;
    meshLayoutInfo.pPushConstantRanges = &meshPushRange;

    if (vkCreatePipelineLayout(_device.getDevice(), &meshLayoutInfo, nullptr,
                               &_meshShaderPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader pipeline layout");
    }

    // ===== 5. Load shaders =====
    VkShaderModule taskShader = Pipeline::loadShaderModule(_device, "shaders/voxel.task.spv");
    VkShaderModule meshShader = Pipeline::loadShaderModule(_device, "shaders/voxel.mesh.spv");
    VkShaderModule fragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");

    // ===== 6. Build mesh shader pipeline =====
    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();

    VkPipelineShaderStageCreateInfo taskStageInfo{};
    taskStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    taskStageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    taskStageInfo.module = taskShader;
    taskStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo meshStageInfo{};
    meshStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshStageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshStageInfo.module = meshShader;
    meshStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragShader;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {taskStageInfo, meshStageInfo, fragStageInfo};

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic rendering
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &drawImage.format;
    renderingInfo.depthAttachmentFormat = depthImage.format;

    // Viewport state (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 3;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = _meshShaderPipelineLayout;

    VkPipeline meshPipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                  &meshPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader pipeline");
    }

    _meshShaderPipeline.init(meshPipeline, _meshShaderPipelineLayout);

    // ===== 7. Create WIREFRAME variant for debugging (F1 toggle) =====
    // Create a second pipeline with VK_POLYGON_MODE_LINE before destroying shader modules
    VkPipelineRasterizationStateCreateInfo wireframeRasterizer = rasterizer;
    wireframeRasterizer.polygonMode = VK_POLYGON_MODE_LINE; // WIREFRAME MODE

    VkGraphicsPipelineCreateInfo wireframePipelineInfo = pipelineInfo;
    wireframePipelineInfo.pRasterizationState = &wireframeRasterizer;

    VkPipeline meshWireframePipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &wireframePipelineInfo,
                                  nullptr, &meshWireframePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader wireframe pipeline");
    }

    _meshShaderWireframePipeline.init(meshWireframePipeline, _meshShaderPipelineLayout);

    // Cleanup shader modules (now that both pipelines are created)
    vkDestroyShaderModule(_device.getDevice(), taskShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), meshShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), fragShader, nullptr);

    // ===== 8. Create and update descriptor sets =====
    // PHASE 1: Create per-frame descriptor sets for mesh shader (double-buffered chunk data)
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _meshShaderDescriptorSets[i] =
            _descriptorAllocator.allocate(_device.getDevice(), _meshShaderSetLayout, nullptr);

        // Note: Index buffer binding will be done later when we have the mega index buffer
        DescriptorWriter meshWriter;
        meshWriter.writeBuffer(0, _cameraUniformBuffer.buffer, sizeof(GPUCameraData), 0,
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        meshWriter.writeBuffer(1, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * _currentMaxChunks, 0,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // Binding 2 (index buffer) will be written after mesh pool initialization

        meshWriter.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
    }

    // Set 1: Fragment shader data (texture atlas) - allocate and bind
    _meshShaderFragDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _meshShaderFragSetLayout, nullptr);

    DescriptorWriter fragWriter;
    fragWriter.writeImage(0, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    fragWriter.writeBuffer(1, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    fragWriter.updateSet(_device.getDevice(), _meshShaderFragDescriptorSet);

    std::cout << "[VoxelRenderer] Mesh shader pipeline initialized successfully!\n";
    std::cout << "[VoxelRenderer] Mesh shader wireframe pipeline created for F1 debug toggle\n";
    std::cout
        << "[VoxelRenderer] NOTE: Index buffer binding will be updated after mesh pool init\n";
}

void VoxelRenderer::ensureBufferCapacity(uint32_t requiredChunks) {
    // Check if we need to resize
    if (requiredChunks <= _currentMaxChunks) {
        return;
    }

    // Calculate new capacity with 1.5x growth factor to reduce reallocation frequency
    uint32_t newCapacity = static_cast<uint32_t>(requiredChunks * 1.5f);

    std::cout << "[VoxelRenderer] Resizing chunk buffers from " << _currentMaxChunks << " to "
              << newCapacity << " chunks\n";

    // NOTE: We must wait because we're about to destroy buffers that may be in use
    // TODO: Optimize by using a deletion queue instead of blocking
    vkDeviceWaitIdle(_device.getDevice());

    // Destroy old buffers
    _bufferManager.destroyBuffer(_indirectBuffer);
    // PHASE 1: Destroy all double-buffered chunk data buffers
    for (auto& buffer : _chunkDataBuffers) {
        _bufferManager.destroyBuffer(buffer);
    }

    // Create new larger buffers
    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * newCapacity,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // PHASE 1: Create double-buffered chunk data buffers
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDataBuffers[i] = _bufferManager.createBuffer(sizeof(GPUChunkData) * newCapacity,
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // PHASE 1: Update per-frame descriptor sets to bind the new chunk data buffers
    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        DescriptorWriter writer;
        writer.writeBuffer(0, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * newCapacity, 0,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(_device.getDevice(), _chunkDescriptorSets[i]);
    }

    // Update current capacity
    _currentMaxChunks = newCapacity;

    // PHASE 5: Buffer was recreated, mark ALL chunks as dirty for re-upload
    _dirtyChunkIndices.clear();
    _dirtyChunkIndices.resize(_chunkDrawInfos.size(), true);
    _dirtyChunkList.clear();
    for (size_t i = 0; i < _chunkDrawInfos.size(); i++) {
        _dirtyChunkList.push_back(static_cast<uint32_t>(i));
    }
    _chunkDataDirty = true;
    _dirtyChunkCount = static_cast<uint32_t>(_chunkDrawInfos.size());
}

void VoxelRenderer::update(const glm::ivec3& cameraChunkPos, int maxLoadDistance) {
    ZoneScoped;
    // Store max load distance for render distance calculation
    _maxLoadDistance = maxLoadDistance;

    // ===== FIX #2: BATCH PROCESSING =====
    // Collect meshes from ALL per-thread queues in a single batch
    // This reduces per-mesh overhead and allows better memory layout
    std::vector<MeshData> meshBatch;
    meshBatch.reserve(MAX_MESHES_PER_BATCH);

    {
        ZoneScopedN("Collect Mesh Batch from All Queues");
        // Iterate through all per-thread queues
        for (auto& queue : _perThreadMeshQueues) {
            while (meshBatch.size() < MAX_MESHES_PER_BATCH) {
                auto meshOpt = queue->try_pop();
                if (!meshOpt.has_value()) {
                    break; // This queue is empty, move to next
                }
                meshBatch.push_back(std::move(meshOpt.value()));
            }
            if (meshBatch.size() >= MAX_MESHES_PER_BATCH) {
                break; // Hit batch limit, process what we have
            }
        }
    }

    // Process the entire batch
    if (!meshBatch.empty()) {
        ZoneScopedN("Process Mesh Batch");
        for (auto& meshData : meshBatch) {
            const glm::ivec3 chunkCoords = meshData.chunkPosition;

            // CRITICAL: Check if chunk is still within acceptable distance
            // If player has moved away, discard this mesh to prevent "ghost chunks"
            const glm::ivec3 offset = chunkCoords - cameraChunkPos;
            const int chebyshevDistance =
                std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

            if (chebyshevDistance > maxLoadDistance) {
                // Chunk is too far away - discard this mesh
                continue;
            }

            if (meshData.vertices.empty() || meshData.indices.empty()) {
                auto it = _chunkDrawLookup.find(chunkCoords);
                if (it != _chunkDrawLookup.end()) {
                    const size_t idx = it->second;

                    // Free the chunk's GPU buffers (INSTANT with VMA!)
                    _meshPool->freeChunkBuffers(_chunkDrawInfos[idx].meshBuffers);

                    if (idx != _chunkDrawInfos.size() - 1) {
                        _chunkDrawInfos[idx] = _chunkDrawInfos.back();
                        _chunkDrawLookup[_chunkDrawInfos[idx].chunkCoords] = idx;
                    }
                    _chunkDrawInfos.pop_back();
                    _chunkDrawLookup.erase(it);
                    _chunkDataDirty = true;
                    _dirtyChunkCount++;
                }
                continue;
            }

            // Allocate dedicated buffers for this chunk via VMA
            ChunkMeshBuffers chunkBuffers;
            {
                ZoneScopedN("Allocate Chunk Buffers");
                chunkBuffers = _meshPool->allocateChunkBuffers(
                    meshData.indices, meshData.vertices,
                    [this](std::function<void(VkCommandBuffer)>&& func) {
                        _executor.immediateSubmit(std::move(func));
                    });
            }

            const glm::vec3 chunkWorldPos{static_cast<float>(chunkCoords[0] * Chunk::CHUNK_SIZE),
                                          static_cast<float>(chunkCoords[1] * Chunk::CHUNK_SIZE),
                                          static_cast<float>(chunkCoords[2] * Chunk::CHUNK_SIZE)};

            if (auto it = _chunkDrawLookup.find(chunkCoords); it != _chunkDrawLookup.end()) {
                // Chunk is being updated - reuse the same index
                const size_t chunkIndex = it->second;
                ChunkDrawInfo& info = _chunkDrawInfos[chunkIndex];
                _meshPool->freeChunkBuffers(info.meshBuffers);

                info.chunkCoords = chunkCoords;
                info.worldPosition = chunkWorldPos;
                info.meshBuffers = chunkBuffers;
                _chunkDataDirty = true;
                _dirtyChunkCount++;

                // PHASE 5: Mark this specific chunk as dirty for incremental upload
                if (chunkIndex >= _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices.resize(chunkIndex + 1, false);
                }
                if (!_dirtyChunkIndices[chunkIndex]) {
                    _dirtyChunkIndices[chunkIndex] = true;
                    _dirtyChunkList.push_back(static_cast<uint32_t>(chunkIndex));
                }
            } else {
                // Try to find an empty slot (unloaded chunk) to reuse
                size_t reuseIndex = _chunkDrawInfos.size();
                for (size_t i = 0; i < _chunkDrawInfos.size(); i++) {
                    if (_chunkDrawInfos[i].meshBuffers.indexCount == 0) {
                        reuseIndex = i;
                        break;
                    }
                }

                size_t chunkIndex;
                if (reuseIndex < _chunkDrawInfos.size()) {
                    // Reuse empty slot
                    ChunkDrawInfo& info = _chunkDrawInfos[reuseIndex];
                    info.chunkCoords = chunkCoords;
                    info.worldPosition = chunkWorldPos;
                    info.meshBuffers = chunkBuffers;
                    _chunkDrawLookup.emplace(chunkCoords, reuseIndex);
                    chunkIndex = reuseIndex;
                } else {
                    // No empty slots, append new
                    chunkIndex = _chunkDrawInfos.size();
                    _chunkDrawInfos.push_back(ChunkDrawInfo{.chunkCoords = chunkCoords,
                                                            .worldPosition = chunkWorldPos,
                                                            .meshBuffers = chunkBuffers});
                    _chunkDrawLookup.emplace(chunkCoords, chunkIndex);
                }
                _chunkDataDirty = true;
                _dirtyChunkCount++;

                // PHASE 5: Mark this specific chunk as dirty for incremental upload
                if (chunkIndex >= _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices.resize(chunkIndex + 1, false);
                }
                if (!_dirtyChunkIndices[chunkIndex]) {
                    _dirtyChunkIndices[chunkIndex] = true;
                    _dirtyChunkList.push_back(static_cast<uint32_t>(chunkIndex));
                }
            }
        }
    }

    // PHASE 3: Submit all batched uploads in a single command buffer
    if (!meshBatch.empty()) {
        ZoneScopedN("Submit Batched Uploads");
        _meshPool->submitPendingUploads([this](std::function<void(VkCommandBuffer)>&& func) {
            _executor.immediateSubmit(std::move(func));
        });
    }

    // ===== FIX #3: UPLOAD THROTTLING =====
    // Only upload if we've accumulated enough changes OR enough time has passed
    const auto now = std::chrono::steady_clock::now();
    const auto msSinceLastUpload =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastUploadTime).count();

    const bool shouldUpload = _chunkDataDirty && _useMeshShaders &&
                              (_dirtyChunkCount >= MIN_CHUNKS_FOR_UPLOAD ||
                               msSinceLastUpload >= MAX_MS_BETWEEN_UPLOADS);

    if (shouldUpload) {
        ZoneScopedN("Upload Chunk Data (GPU-Persistent)");

        // PHASE 1 OPTIMIZATION: Double-buffered chunk data eliminates vkDeviceWaitIdle!
        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);

        // Ensure buffers are large enough for total chunk count
        ensureBufferCapacity(static_cast<uint32_t>(_chunkDrawInfos.size()));

        // PHASE 5: Build full chunk data array (needed for shader indexing)
        _chunkDrawData.clear();
        _chunkDrawData.reserve(_chunkDrawInfos.size());

        for (const ChunkDrawInfo& drawInfo : _chunkDrawInfos) {
            const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;

            GPUChunkData chunkData{};
            chunkData.chunkWorldPos = drawInfo.worldPosition;
            chunkData.indexCount = buffers.indexCount;
            chunkData.vertexBufferAddress = buffers.vertexAddress;
            chunkData.firstIndex = buffers.firstIndex;
            chunkData._padding = 0;
            _chunkDrawData.push_back(chunkData);
        }

        if (!_chunkDrawData.empty()) {
            // PHASE 5: Upload only dirty chunks for efficiency
            if (!_dirtyChunkList.empty() && _dirtyChunkList.size() < _chunkDrawData.size()) {
                ZoneScopedN("Upload Dirty Chunks Only");

                // PHASE 6: Acquire staging buffer from pool instead of creating/destroying
                const VkDeviceSize stagingSize = _dirtyChunkList.size() * sizeof(GPUChunkData);
                AllocatedBuffer stagingBuffer = _meshPool->acquireStagingBuffer(
                    stagingSize, MeshBufferPool::StagingType::Generic);

                // Copy dirty chunk data to staging buffer
                std::vector<GPUChunkData> dirtyData;
                dirtyData.reserve(_dirtyChunkList.size());
                for (uint32_t idx : _dirtyChunkList) {
                    if (idx < _chunkDrawData.size()) {
                        dirtyData.push_back(_chunkDrawData[idx]);
                    }
                }

                _bufferManager.uploadToBuffer(stagingBuffer, dirtyData.data(),
                                              dirtyData.size() * sizeof(GPUChunkData));

                // Copy each dirty chunk to its position in the GPU buffer
                _executor.immediateSubmit([&](VkCommandBuffer cmd) {
                    for (size_t i = 0; i < _dirtyChunkList.size(); i++) {
                        const uint32_t chunkIndex = _dirtyChunkList[i];
                        if (chunkIndex >= _chunkDrawData.size())
                            continue;

                        VkBufferCopy copyRegion{};
                        copyRegion.srcOffset = i * sizeof(GPUChunkData);
                        copyRegion.dstOffset = chunkIndex * sizeof(GPUChunkData);
                        copyRegion.size = sizeof(GPUChunkData);

                        vkCmdCopyBuffer(cmd, stagingBuffer.buffer,
                                        _chunkDataBuffers[frameIndex].buffer, 1, &copyRegion);
                    }
                });

                // PHASE 6: DON'T destroy staging buffer - pool manages it automatically!
                // The buffer will be recycled after GPU is done (via updateStagingPools)

                std::cout << "[VoxelRenderer] PHASE 5+6: Uploaded " << _dirtyChunkList.size()
                          << " dirty chunks (pooled staging)\n";
            } else {
                // Full upload (initial state or buffer resize)
                ZoneScopedN("Upload All Chunks");
                _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                              _chunkDrawData.size() * sizeof(GPUChunkData));

                std::cout << "[VoxelRenderer] Uploaded all " << _chunkDrawData.size()
                          << " chunks (full update)\n";
            }

            // Clear dirty tracking
            for (uint32_t idx : _dirtyChunkList) {
                if (idx < _dirtyChunkIndices.size()) {
                    _dirtyChunkIndices[idx] = false;
                }
            }
            _dirtyChunkList.clear();
        }

        // Reset throttling counters
        _chunkDataDirty = false;
        _dirtyChunkCount = 0;
        _lastUploadTime = now;
    }
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {
    ZoneScopedN("VoxelRenderer::drawVoxels");
    // Process deletion queue at the start of each frame
    // Buffers queued for deletion are now safe to destroy
    {
        ZoneScopedN("Process Deletion Queue");
        _meshPool->processDeletionQueue();
    }

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();
    VkExtent2D drawExtent = _context.getDrawExtent();

    // Build chunk data ONLY for traditional path (mesh shader path builds in update())
    if (!_useMeshShaders) {
        ZoneScopedN("Build Chunk Data (Traditional)");
        _chunkDrawData.clear();
        _chunkDrawData.reserve(_chunkDrawInfos.size());

        for (const ChunkDrawInfo& drawInfo : _chunkDrawInfos) {
            const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;
            if (buffers.indexCount == 0) {
                continue;
            }

            // Build GPU chunk data with all necessary info
            GPUChunkData chunkData{};
            chunkData.chunkWorldPos = drawInfo.worldPosition;
            chunkData.indexCount = buffers.indexCount;
            chunkData.vertexBufferAddress = buffers.vertexAddress;
            chunkData.firstIndex = buffers.firstIndex; // CRITICAL for compute shader!
            chunkData._padding = 0;
            _chunkDrawData.push_back(chunkData);
        }
    }

    // Early exit if nothing to draw
    if (_chunkDrawData.empty()) {
        return;
    }

    // Ensure buffers are large enough
    ensureBufferCapacity(static_cast<uint32_t>(_chunkDrawData.size()));

    // PHASE 1: Upload chunk data to current frame's buffer (traditional path)
    {
        ZoneScopedN("Upload Chunk Data");
        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
        _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                      _chunkDrawData.size() * sizeof(GPUChunkData));
    }

    // ===== GPU FRUSTUM CULLING COMPUTE PASS =====
    if (_enableGPUCulling) {
        ZoneScopedN("GPU Frustum Culling");

        // 1. Build and upload frustum data (with caching optimization)
        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(80.0F),
                                                static_cast<float>(drawExtent.width) /
                                                    static_cast<float>(drawExtent.height),
                                                0.1F, 10000.0F);
        projection[1][1] *= -1.0F; // Flip Y for Vulkan

        glm::vec3 cameraPos = camera.getPosition();

        // ALWAYS rebuild frustum data (cache was broken - didn't track view matrix rotation)
        // This is cheap: ~6 plane calculations + 140 byte upload per frame
        std::array<glm::vec4, 6> frustumPlanes;
        {
            ZoneScopedN("Extract Frustum Planes");
            frustumPlanes = camera.getFrustumPlanes(projection);
        }

        GPUFrustumData frustumData{};
        for (size_t i = 0; i < 6; ++i) {
            frustumData.planes[i] = frustumPlanes[i];
        }
        frustumData.cameraPos = cameraPos;
        // Dynamic render distance based on actual load radius + margin
        // Add 2 chunks margin to prevent pop-in at distance boundary
        const float renderDistanceChunks = static_cast<float>(_maxLoadDistance + 2);
        frustumData.maxRenderDistance = renderDistanceChunks * 32.0F; // chunks * chunkSize

        {
            ZoneScopedN("Upload Frustum Data");
            _bufferManager.uploadToBuffer(_frustumUniformBuffer, &frustumData,
                                          sizeof(GPUFrustumData));
        }

        // 2. Reset atomic counter (drawCount = 0) using GPU command
        // vkCmdFillBuffer is faster than CPU upload (no staging buffer needed)
        {
            ZoneScopedN("Reset Draw Counter");
            vkCmdFillBuffer(cmd, _culledIndirectBuffer.buffer, 0, sizeof(uint32_t), 0);

            // Memory barrier: ensure fill completes before compute reads/writes
            VkMemoryBarrier fillBarrier{};
            fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fillBarrier, 0, nullptr,
                                 0, nullptr);
        }

        // 3. Bind compute pipeline and descriptor set
        {
            ZoneScopedN("Bind Compute Pipeline");
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _frustumCullPipeline.getPipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _frustumCullPipelineLayout, 0,
                                    1, &_frustumCullDescriptorSet, 0, nullptr);
        }

        // 4. Push constants for compute shader
        ComputePushConstants pushConstants{};
        pushConstants.totalChunks = static_cast<uint32_t>(_chunkDrawData.size());
        pushConstants.chunkSize = 32.0f; // CHUNK_SIZE
        // Debug mode: 0=all tests, 1=skip distance, 2=skip sphere, 4=skip AABB, 7=skip all
        // Enable frustum tests (sphere+AABB), disable distance culling
        // Bit 0 set = skip distance culling (no circular effect)
        // Bits 1,2 clear = use sphere and AABB frustum tests
        pushConstants.debugMode = 1; // Skip distance, use frustum only
        pushConstants._padding2 = 0;

        vkCmdPushConstants(cmd, _frustumCullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(ComputePushConstants), &pushConstants);

        // 5. Dispatch compute shader (64 threads per workgroup)
        {
            ZoneScopedN("Compute Dispatch");
            const uint32_t workgroupCount = (pushConstants.totalChunks + 63) / 64;
            vkCmdDispatch(cmd, workgroupCount, 1, 1);
        }

        // 6. Memory barrier: compute write → indirect read
        VkMemoryBarrier memoryBarrier{};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

        // Update statistics (will be populated later via readback if needed)
        _cullingStats.totalChunks = pushConstants.totalChunks;
        _cullingStats.visibleChunks = 0; // Will be updated by GPU
        _cullingStats.culledChunks = 0;
        _cullingStats.cullingPercentage = 0.0f;

        // Debug: Print GPU culling info every 60 frames (once per second at 60 FPS)
        static uint32_t frameCounter = 0;
        if (++frameCounter >= 60) {
            std::cout << "\n[GPU Culling] ===== DEBUG INFO =====\n";
            std::cout << "[GPU Culling] Processing " << pushConstants.totalChunks
                      << " chunks, chunk size: " << pushConstants.chunkSize << "m\n";
            std::cout << "[GPU Culling] Debug mode: " << pushConstants.debugMode
                      << " (0=all tests, 7=skip all)\n";

            // Camera info
            glm::vec3 forward = camera.getFront();
            std::cout << "[GPU Culling] Camera pos: (" << cameraPos.x << ", " << cameraPos.y << ", "
                      << cameraPos.z << ")\n";
            std::cout << "[GPU Culling] Camera forward: (" << forward.x << ", " << forward.y << ", "
                      << forward.z << ")\n";
            std::cout << "[GPU Culling] Max render distance: " << frustumData.maxRenderDistance
                      << "m\n";

            // Frustum planes (all 6)
            const char* planeNames[] = {"Left", "Right", "Top", "Bottom", "Near", "Far"};
            std::cout << "[GPU Culling] Frustum planes (nx, ny, nz, d):\n";
            for (int i = 0; i < 6; i++) {
                const glm::vec4& p = frustumPlanes[i];
                std::cout << "  " << planeNames[i] << ": (" << p.x << ", " << p.y << ", " << p.z
                          << ", " << p.w << ")\n";
            }

            if (!_chunkDrawInfos.empty()) {
                const auto& sampleChunk = _chunkDrawInfos[0];
                std::cout << "[GPU Culling] Sample chunk pos: (" << sampleChunk.worldPosition.x
                          << ", " << sampleChunk.worldPosition.y << ", "
                          << sampleChunk.worldPosition.z << ")\n";

                // CPU-side verification: calculate distance to sample chunk
                glm::vec3 chunkCenter =
                    sampleChunk.worldPosition + glm::vec3(16.0f); // halfChunkSize
                glm::vec3 toChunk = chunkCenter - cameraPos;
                float distToChunk = glm::length(toChunk);
                std::cout << "[GPU Culling] Sample chunk center: (" << chunkCenter.x << ", "
                          << chunkCenter.y << ", " << chunkCenter.z << ")\n";
                std::cout << "[GPU Culling] Distance to sample chunk: " << distToChunk << "m\n";
                std::cout << "[GPU Culling] Distance culling should: "
                          << (distToChunk <= frustumData.maxRenderDistance ? "PASS" : "FAIL")
                          << "\n";

                // Test frustum plane distances
                float chunkRadius = 16.0f * 1.732051f; // halfChunkSize * sqrt(3)
                for (int i = 0; i < 6; i++) {
                    const glm::vec4& p = frustumPlanes[i];
                    float dist = glm::dot(glm::vec3(p), chunkCenter) + p.w;
                    const char* planeNames[] = {"Left", "Right", "Top", "Bottom", "Near", "Far"};
                    bool pass =
                        (dist <=
                         chunkRadius); // Negated planes: negative=inside, cull if dist > radius
                    std::cout << "  " << planeNames[i] << " plane distance: " << dist
                              << " (radius: " << chunkRadius
                              << ", should: " << (pass ? "PASS" : "FAIL") << ")\n";
                }
            }
            std::cout << "[GPU Culling] =====================\n\n";
            frameCounter = 0;
        }
    } else {
        // CPU fallback path (for debugging)
        _indirectCommands.clear();
        _indirectCommands.reserve(_chunkDrawData.size());

        for (const auto& drawInfo : _chunkDrawInfos) {
            const ChunkMeshBuffers& buffers = drawInfo.meshBuffers;
            if (buffers.indexCount == 0)
                continue;

            VkDrawIndexedIndirectCommand indirectCmd{};
            indirectCmd.indexCount = buffers.indexCount;
            indirectCmd.instanceCount = 1;
            indirectCmd.firstIndex = buffers.firstIndex;
            indirectCmd.vertexOffset = 0;
            indirectCmd.firstInstance = 0;
            _indirectCommands.push_back(indirectCmd);
        }

        _bufferManager.uploadToBuffer(_indirectBuffer, _indirectCommands.data(),
                                      _indirectCommands.size() *
                                          sizeof(VkDrawIndexedIndirectCommand));
    }

    VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = drawImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // Load the sky that was already rendered
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {0.1F, 0.2F, 0.3F, 1.0F}}}};

    VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depthImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // Load the depth buffer from sky rendering
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {.depth = 1.0F, .stencil = 0}}};

    VkRenderingInfo renderInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .pNext = nullptr,
                               .flags = 0,
                               .renderArea = {.offset = {0, 0}, .extent = drawExtent},
                               .layerCount = 1,
                               .viewMask = 0,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &colorAttachment,
                               .pDepthAttachment = &depthAttachment,
                               .pStencilAttachment = nullptr};

    vkCmdBeginRendering(cmd, &renderInfo);

    // Set viewport and scissor
    VkViewport viewport{.x = 0.0F,
                        .y = 0.0F,
                        .width = static_cast<float>(drawExtent.width),
                        .height = static_cast<float>(drawExtent.height),
                        .minDepth = 0.0F,
                        .maxDepth = 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{.offset = {0, 0}, .extent = drawExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // ===== MESH SHADER PATH vs TRADITIONAL PATH =====
    if (_useMeshShaders) {
        ZoneScopedN("Mesh Shader Rendering");
        TracyVkZone(_device.getTracyCtx(), cmd, "GPU Mesh Shader Rendering");

        // Bind mesh shader pipeline (wireframe or filled based on F1 toggle)
        VkPipeline activePipeline = wireframeMode ? _meshShaderWireframePipeline.getPipeline()
                                                  : _meshShaderPipeline.getPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

        // PHASE 1: Bind per-frame descriptor sets for double-buffered chunk data
        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
        VkDescriptorSet descriptorSets[] = {_meshShaderDescriptorSets[frameIndex], _meshShaderFragDescriptorSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshShaderPipelineLayout, 0,
                                2, descriptorSets, 0, nullptr);

        // Set up camera data for UBO
        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(80.0F),
                                                static_cast<float>(drawExtent.width) /
                                                    static_cast<float>(drawExtent.height),
                                                0.1F, 10000.0F);
        projection[1][1] *= -1.0F;

        GPUCameraData cameraData{};
        cameraData.viewProjection = projection * view;

        // Extract frustum planes
        std::array<glm::vec4, 6> frustumPlanes = camera.getFrustumPlanes(projection);
        for (size_t i = 0; i < 6; ++i) {
            cameraData.planes[i] = frustumPlanes[i];
        }
        cameraData.cameraPos = camera.getPosition();
        // Dynamic render distance based on actual load radius + margin
        const float renderDistanceChunks = static_cast<float>(_maxLoadDistance + 2);
        cameraData.maxRenderDistance = renderDistanceChunks * 32.0F; // chunks * chunkSize

        // Upload camera data to UBO
        {
            ZoneScopedN("Upload Camera UBO");
            _bufferManager.uploadToBuffer(_cameraUniformBuffer, &cameraData, sizeof(GPUCameraData));
        }

        // Push constants for task shader
        struct MeshShaderPushConstants {
            uint32_t totalChunks;
            float chunkSize;
            uint32_t maxVerticesPerMeshWorkgroup;
            uint32_t maxMeshWorkgroupsPerTask;
        };

        MeshShaderPushConstants pushConstants{};
        pushConstants.totalChunks = static_cast<uint32_t>(_chunkDrawData.size());
        pushConstants.chunkSize = 32.0F;
        pushConstants.maxVerticesPerMeshWorkgroup = 256; // Must match mesh shader max_vertices
        pushConstants.maxMeshWorkgroupsPerTask = _maxMeshWorkgroupsPerTask;

        // DEBUG: Log chunk geometry stats (only once per second to avoid spam)
        static auto lastDebugTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDebugTime).count() >= 1) {
            std::cout << "[VoxelRenderer] Total chunks: " << pushConstants.totalChunks << "\n";
            std::cout << "[VoxelRenderer] Task workgroups to dispatch: "
                      << ((pushConstants.totalChunks + 31) / 32) << "\n";
            std::cout << "[VoxelRenderer] maxMeshWorkgroupsPerTask: " << _maxMeshWorkgroupsPerTask
                      << "\n";

            // Log detailed info for first few chunks
            std::cout << "[VoxelRenderer] First 5 chunks:\n";
            for (size_t i = 0; i < std::min(size_t(5), _chunkDrawData.size()); i++) {
                const auto& chunk = _chunkDrawData[i];
                uint32_t triangles = chunk.indexCount / 3;
                uint32_t expectedTiles = (triangles + 84) / 85;
                std::cout << "  Chunk " << i << ": indexCount=" << chunk.indexCount
                          << " triangles=" << triangles << " expectedTiles=" << expectedTiles
                          << "\n";
            }

            // Find and log stats for largest chunks
            uint32_t maxIndexCount = 0;
            for (const auto& chunk : _chunkDrawData) {
                maxIndexCount = std::max(maxIndexCount, chunk.indexCount);
            }
            if (maxIndexCount > 0) {
                uint32_t maxTriangles = maxIndexCount / 3;
                uint32_t expectedTiles = (maxTriangles + 84) / 85;
                std::cout << "[VoxelRenderer] Largest chunk: " << maxIndexCount << " indices, "
                          << maxTriangles << " triangles, ~" << expectedTiles
                          << " expected tiles\n";
            }
            lastDebugTime = now;
        }

        vkCmdPushConstants(cmd, _meshShaderPipelineLayout, VK_SHADER_STAGE_TASK_BIT_EXT, 0,
                           sizeof(MeshShaderPushConstants), &pushConstants);

        // Dispatch mesh shader tasks
        // PHASE 1 FIX: Changed from 32 to 16 chunks per task workgroup
        // Each task workgroup (16 threads) handles up to 16 chunks
        // This reduces tile density per workgroup, preventing payload overflow
        // With 16 chunks @ 128 avg tiles each = 2048 tiles (at limit)
        // vs 32 chunks @ 64 avg tiles each = 2048 tiles (easier to overflow)
        {
            ZoneScopedN("Dispatch Mesh Tasks");
            uint32_t taskWorkgroups = (pushConstants.totalChunks + 15) / 16; // PHASE 1 FIX: 31->15, /32->/16
            _vkCmdDrawMeshTasksEXT(cmd, taskWorkgroups, 1, 1);
        }

    } else {
        // Traditional vertex/fragment shader path
        ZoneScopedN("Traditional Rendering");
        TracyVkZone(_device.getTracyCtx(), cmd, "GPU Traditional Rendering");

        // Bind pipeline based on wireframe mode
        VkPipeline activePipeline =
            wireframeMode ? _voxelWireframePipeline.getPipeline() : _voxelPipeline.getPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

        // PHASE 1: Bind per-frame descriptor set for double-buffered chunk data
        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
        VkDescriptorSet activeChunkSet =
            _enableGPUCulling ? _culledChunkDescriptorSet : _chunkDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _voxelPipeline.getLayout(), 0,
                                1, &activeChunkSet, 0, nullptr);

        // Set up view-projection matrix
        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(80.0F),
                                                static_cast<float>(drawExtent.width) /
                                                    static_cast<float>(drawExtent.height),
                                                0.1F, 10000.0F);
        projection[1][1] *= -1.0F;
        glm::mat4 viewProjection = projection * view;

        // Push constants
        vkCmdPushConstants(cmd, _voxelPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(glm::mat4), &viewProjection);

        // Bind mega index buffer
        VkBuffer indexBuffer = _meshPool->getIndexBuffer();
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Multi-Draw Indirect
        if (_enableGPUCulling && _supportsDrawIndirectCount) {
            vkCmdDrawIndexedIndirectCount(cmd, _culledIndirectBuffer.buffer, sizeof(uint32_t),
                                          _culledIndirectBuffer.buffer, 0,
                                          static_cast<uint32_t>(_chunkDrawData.size()),
                                          sizeof(VkDrawIndexedIndirectCommand));
        } else if (_enableGPUCulling) {
            vkCmdDrawIndexedIndirect(cmd, _culledIndirectBuffer.buffer, sizeof(uint32_t),
                                     static_cast<uint32_t>(_chunkDrawData.size()),
                                     sizeof(VkDrawIndexedIndirectCommand));
        } else {
            vkCmdDrawIndexedIndirect(cmd, _indirectBuffer.buffer, 0,
                                     static_cast<uint32_t>(_indirectCommands.size()),
                                     sizeof(VkDrawIndexedIndirectCommand));
        }
    }

    vkCmdEndRendering(cmd);
}

void VoxelRenderer::rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks) {
    if (unloadedChunks.empty()) {
        return;
    }

    // ========================================================================
    // VMA PER-CHUNK BUFFERS = INSTANT UNLOAD!
    // ========================================================================

    // Process each chunk to unload
    for (const glm::ivec3& pos : unloadedChunks) {
        auto it = _chunkDrawLookup.find(pos);
        if (it == _chunkDrawLookup.end()) {
            continue; // Chunk not loaded in renderer
        }

        const size_t idx = it->second;
        ChunkDrawInfo& info = _chunkDrawInfos[idx];

        // Free the chunk's GPU buffers - INSTANT! No GPU stall!
        _meshPool->freeChunkBuffers(info.meshBuffers);

        // IMPORTANT: Don't remove from _chunkDrawInfos array to keep indices stable!
        // Instead, mark as empty by zeroing the index count and clearing buffer handles
        // This ensures GPU buffer indices remain valid and prevents double-free in destructor
        info.meshBuffers.indexCount = 0;
        info.meshBuffers.vertexCount = 0;
        info.meshBuffers.vertexAddress = 0;
        info.meshBuffers.firstIndex = 0;
        info.meshBuffers.vertexBuffer.buffer = VK_NULL_HANDLE;
        info.meshBuffers.vertexBuffer.allocation = VK_NULL_HANDLE;

        // Remove from lookup map (but keep slot in array)
        _chunkDrawLookup.erase(it);
        _chunkDataDirty = true; // Mark for GPU upload
    }

    // Reset index buffer offset if all chunks are unloaded
    // This prevents index buffer exhaustion during long play sessions
    if (_chunkDrawInfos.empty()) {
        _meshPool->resetIndexOffset();
    }
}

float VoxelRenderer::getMeshPoolUsage() const {
    if (!_meshPool) {
        return 0.0f;
    }
    // Return memory usage as a fraction based on current vs max index buffer capacity
    // This gives a more accurate representation of how full our buffers are
    const size_t totalIndexBytes = _meshPool->getTotalIndexMemory();
    const VkDeviceSize indexCapacity = _meshPool->getIndexBufferCapacity();

    if (indexCapacity == 0) {
        return 0.0f;
    }

    // Calculate usage as percentage of index buffer capacity (the main bottleneck)
    return std::min(1.0f, static_cast<float>(totalIndexBytes) / static_cast<float>(indexCapacity));
}

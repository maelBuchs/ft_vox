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
    // Free all chunk vertex buffers before clearing vectors
    // VMA requires explicit deallocation before the allocator is destroyed

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

    // Wait for GPU to finish before destroying resources
    // This prevents "pipeline layout in use" validation errors
    // Even though FrameManager waits for fences, we add a safety check here
    vkDeviceWaitIdle(_device.getDevice());

    _voxelPipeline.cleanup(_device);
    _voxelWireframePipeline.cleanup(_device);
    _meshShaderPipeline.cleanup(_device);
    _meshShaderWireframePipeline.cleanup(_device);

    // Clean up compute culling pipeline
    _frustumCullPipeline.cleanup(_device);

    // Wait again after pipeline destruction to ensure they're fully destroyed
    // before destroying the layouts they reference. This prevents the layout leak
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

    // Set up callback to update mesh shader descriptors when index buffer is resized
    if (_useMeshShaders) {
        _meshPool->setIndexBufferResizeCallback([this]() {
            _indexBufferResizePending = true;
        });

        // Write initial index buffer binding (Binding 2) now that mesh pool exists
        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter writer;
            writer.writeBuffer(2, _meshPool->getIndexBuffer(),
                              _meshPool->getIndexBufferCapacity(), 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }
    }

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

        // Vertices are accessed via buffer_reference in the shader
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
    }

    if (_useMeshShaders && _meshPool) {
        VkBuffer indexBuffer = _meshPool->getIndexBuffer();
        VkDeviceSize indexBufferSize = _meshPool->getIndexBufferCapacity();

        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter indexWriter;
            indexWriter.writeBuffer(2, indexBuffer, indexBufferSize, 0,
                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            indexWriter.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }
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

    // Create descriptor set layout for compute shader
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

    VkPushConstantRange computePushRange{};
    computePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computePushRange.offset = 0;
    computePushRange.size = sizeof(ComputePushConstants);

    ComputePipelineBuilder computeBuilder;
    computeBuilder.setShader("shaders/frustum_cull.comp.spv");
    computeBuilder.setDescriptorSetLayout(_frustumCullSetLayout);
    computeBuilder.setPushConstantRange(computePushRange);

    ComputePipelineBuilder::BuildResult buildResult = computeBuilder.build(_device);

    _frustumCullPipelineLayout = buildResult.layout;
    _frustumCullPipeline.init(buildResult.pipeline, _frustumCullPipelineLayout);

    _frustumUniformBuffer = _bufferManager.createBuffer(sizeof(GPUFrustumData),
                                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VMA_MEMORY_USAGE_CPU_TO_GPU);

    const size_t indirectBufferSize =
        sizeof(uint32_t) + (sizeof(VkDrawIndexedIndirectCommand) * _currentMaxChunks);
    _culledIndirectBuffer = _bufferManager.createBuffer(indirectBufferSize,
                                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VMA_MEMORY_USAGE_GPU_ONLY);

    _culledChunkDataBuffer = _bufferManager.createBuffer(sizeof(GPUChunkData) * _currentMaxChunks,
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_GPU_ONLY);

    _frustumCullDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _frustumCullSetLayout, nullptr);

    DescriptorWriter computeWriter;
    computeWriter.writeBuffer(0, _chunkDataBuffers[0].buffer, sizeof(GPUChunkData) * _currentMaxChunks,
                              0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    computeWriter.writeBuffer(1, _frustumUniformBuffer.buffer, sizeof(GPUFrustumData), 0,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    computeWriter.writeBuffer(2, _culledIndirectBuffer.buffer, indirectBufferSize, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    computeWriter.writeBuffer(3, _culledChunkDataBuffer.buffer,
                              sizeof(GPUChunkData) * _currentMaxChunks, 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    computeWriter.updateSet(_device.getDevice(), _frustumCullDescriptorSet);

    _culledChunkDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _chunkSetLayout, nullptr);

    DescriptorWriter culledWriter;
    culledWriter.writeBuffer(0, _culledChunkDataBuffer.buffer,
                             sizeof(GPUChunkData) * _currentMaxChunks, 0,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    // TODO: optimize by reusing descriptors
    culledWriter.updateSet(_device.getDevice(), _culledChunkDescriptorSet);

    VkPhysicalDeviceVulkan12Features supportedFeatures12{};
    supportedFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &supportedFeatures12;

    vkGetPhysicalDeviceFeatures2(_device.getPhysicalDevice(), &features2);

    _supportsDrawIndirectCount = (supportedFeatures12.drawIndirectCount == VK_TRUE);
}

void VoxelRenderer::initMeshShaderPipeline(VkImageView atlasView, VkSampler atlasSampler) {
    ZoneScoped;

    // Initialize camera UBO to null first to prevent corruption
    _cameraUniformBuffer.buffer = VK_NULL_HANDLE;
    _cameraUniformBuffer.allocation = VK_NULL_HANDLE;

    if (!_device.supportsMeshShaders()) {
        _useMeshShaders = false;
        return;
    }

    _useMeshShaders = true;

    _vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        vkGetDeviceProcAddr(_device.getDevice(), "vkCmdDrawMeshTasksEXT"));

    if (_vkCmdDrawMeshTasksEXT == nullptr) {
        _useMeshShaders = false;
        return;
    }

    VkPhysicalDeviceMeshShaderPropertiesEXT meshProps{};
    meshProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    meshProps.pNext = nullptr;

    VkPhysicalDeviceProperties2 deviceProps{};
    deviceProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProps.pNext = &meshProps;
    vkGetPhysicalDeviceProperties2(_device.getPhysicalDevice(), &deviceProps);

    // Validate mesh shader limits against our shader requirements
    // Our shaders require: max_vertices = 256, max_primitives = 85, task local_size_x = 16
    const uint32_t REQUIRED_MAX_VERTICES = 256;
    const uint32_t REQUIRED_MAX_PRIMITIVES = 85;
    const uint32_t REQUIRED_TASK_INVOCATIONS = 16;
    const uint32_t MIN_MESH_WORKGROUPS = 2048;

    if (meshProps.maxMeshOutputVertices < REQUIRED_MAX_VERTICES ||
        meshProps.maxMeshOutputPrimitives < REQUIRED_MAX_PRIMITIVES ||
        meshProps.maxTaskWorkGroupInvocations < REQUIRED_TASK_INVOCATIONS ||
        meshProps.maxMeshWorkGroupCount[0] < MIN_MESH_WORKGROUPS) {
        std::cerr << "[VoxelRenderer] Mesh shader limits insufficient for this application:\n"
                  << "  maxMeshOutputVertices: " << meshProps.maxMeshOutputVertices
                  << " (required: " << REQUIRED_MAX_VERTICES << ")\n"
                  << "  maxMeshOutputPrimitives: " << meshProps.maxMeshOutputPrimitives
                  << " (required: " << REQUIRED_MAX_PRIMITIVES << ")\n"
                  << "  maxTaskWorkGroupInvocations: " << meshProps.maxTaskWorkGroupInvocations
                  << " (required: " << REQUIRED_TASK_INVOCATIONS << ")\n"
                  << "  maxMeshWorkGroupCount[0]: " << meshProps.maxMeshWorkGroupCount[0]
                  << " (required: " << MIN_MESH_WORKGROUPS << ")\n";
        _useMeshShaders = false;
        return;
    }

    // Validate task payload size (TaskPayloadData struct: 4 bytes meshCount + 2047 * 8 bytes entries)
    const uint32_t REQUIRED_TASK_PAYLOAD_SIZE = 16380;
    if (meshProps.maxTaskPayloadSize < REQUIRED_TASK_PAYLOAD_SIZE) {
        std::cerr << "[VoxelRenderer] Task payload size insufficient:\n"
                  << "  maxTaskPayloadSize: " << meshProps.maxTaskPayloadSize
                  << " (required: " << REQUIRED_TASK_PAYLOAD_SIZE << ")\n";
        _useMeshShaders = false;
        return;
    }

    // Log all mesh shader properties for debugging
    std::cout << "[VoxelRenderer] Mesh Shader Properties:\n"
              << "  maxTaskPayloadSize: " << meshProps.maxTaskPayloadSize << " bytes\n"
              << "  maxMeshOutputVertices: " << meshProps.maxMeshOutputVertices << "\n"
              << "  maxMeshOutputPrimitives: " << meshProps.maxMeshOutputPrimitives << "\n"
              << "  maxTaskWorkGroupInvocations: " << meshProps.maxTaskWorkGroupInvocations << "\n"
              << "  maxMeshWorkGroupInvocations: " << meshProps.maxMeshWorkGroupInvocations << "\n"
              << "  maxPreferredTaskWorkGroupInvocations: "
              << meshProps.maxPreferredTaskWorkGroupInvocations << "\n"
              << "  maxPreferredMeshWorkGroupInvocations: "
              << meshProps.maxPreferredMeshWorkGroupInvocations << "\n"
              << "  maxMeshWorkGroupCount[0]: " << meshProps.maxMeshWorkGroupCount[0] << "\n"
              << "  maxTaskWorkGroupCount[0]: " << meshProps.maxTaskWorkGroupCount[0] << "\n"
              << "  maxTaskSharedMemorySize: " << meshProps.maxTaskSharedMemorySize << " bytes\n"
              << "  maxMeshSharedMemorySize: " << meshProps.maxMeshSharedMemorySize << " bytes\n";

    _maxMeshWorkgroupsPerTask = std::max(meshProps.maxMeshWorkGroupCount[0], 1u);

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

    _cameraUniformBuffer = _bufferManager.createBuffer(sizeof(GPUCameraData),
                                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_CPU_TO_GPU);

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

    VkShaderModule taskShader = Pipeline::loadShaderModule(_device, "shaders/voxel.task.spv");
    VkShaderModule meshShader = Pipeline::loadShaderModule(_device, "shaders/voxel.mesh.spv");
    VkShaderModule fragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");

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

    // Create WIREFRAME variant for debugging (F1 toggle)
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

    _meshShaderFragDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _meshShaderFragSetLayout, nullptr);

    DescriptorWriter fragWriter;
    fragWriter.writeImage(0, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    fragWriter.writeBuffer(1, _atlasConfigBuffer.buffer, sizeof(int), 0,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    fragWriter.updateSet(_device.getDevice(), _meshShaderFragDescriptorSet);
}

void VoxelRenderer::ensureBufferCapacity(uint32_t requiredChunks) {
    // Check if we need to resize
    if (requiredChunks <= _currentMaxChunks) {
        return;
    }

    uint32_t newCapacity = static_cast<uint32_t>(requiredChunks * 1.5f);

    // TODO: Optimize by using a deletion queue instead of blocking
    vkDeviceWaitIdle(_device.getDevice());

    _bufferManager.destroyBuffer(_indirectBuffer);
    for (auto& buffer : _chunkDataBuffers) {
        _bufferManager.destroyBuffer(buffer);
    }

    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * newCapacity,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        _chunkDataBuffers[i] = _bufferManager.createBuffer(sizeof(GPUChunkData) * newCapacity,
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
        DescriptorWriter writer;
        writer.writeBuffer(0, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * newCapacity, 0,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(_device.getDevice(), _chunkDescriptorSets[i]);
    }

    if (_useMeshShaders) {
        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter writer;
            writer.writeBuffer(1, _chunkDataBuffers[i].buffer, sizeof(GPUChunkData) * newCapacity, 0,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }
    }

    _currentMaxChunks = newCapacity;

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
    _maxLoadDistance = maxLoadDistance;

    {
        const auto now = std::chrono::steady_clock::now();
        const float frameTimeMs = std::chrono::duration<float, std::milli>(now - _lastFrameTime).count();
        _lastFrameTime = now;

        // Exponential moving average (smooth out spikes)
        _avgFrameTimeMs = _avgFrameTimeMs * 0.95f + frameTimeMs * 0.05f;

        // Calculate total pending meshes across all queues
        size_t totalPending = 0;
        for (const auto& queue : _perThreadMeshQueues) {
            totalPending += queue->size();
        }

        // Adapt batch size based on performance and queue depth
        if (_avgFrameTimeMs < 10.0f && totalPending > 500) {
            // Running fast with large backlog - be aggressive
            _adaptiveMaxMeshes = 512;
            _adaptiveMinChunks = 8;
            _adaptiveMaxMs = 8;
        } else if (_avgFrameTimeMs < 16.67f && totalPending > 100) {
            // Running at 60+ FPS with moderate backlog - increase throughput
            _adaptiveMaxMeshes = 384;
            _adaptiveMinChunks = 16;
            _adaptiveMaxMs = 12;
        } else if (_avgFrameTimeMs > 25.0f) {
            // Running slow (<40 FPS) - reduce load
            _adaptiveMaxMeshes = 128;
            _adaptiveMinChunks = 64;
            _adaptiveMaxMs = 24;
        } else {
            // Normal operation - use defaults
            _adaptiveMaxMeshes = MAX_MESHES_PER_BATCH;
            _adaptiveMinChunks = MIN_CHUNKS_FOR_UPLOAD;
            _adaptiveMaxMs = MAX_MS_BETWEEN_UPLOADS;
        }
    }

    std::vector<MeshData> meshBatch;
    meshBatch.reserve(_adaptiveMaxMeshes);

    {
        ZoneScopedN("Collect Mesh Batch from All Queues");
        for (auto& queue : _perThreadMeshQueues) {
            const size_t remaining = _adaptiveMaxMeshes - meshBatch.size();
            if (remaining == 0) {
                break;
            }

            size_t extracted = queue->try_pop_batch(meshBatch, remaining);
            (void)extracted;

            if (meshBatch.size() >= _adaptiveMaxMeshes) {
                break;
            }
        }
    }

    // Process the entire batch
    if (!meshBatch.empty()) {
        ZoneScopedN("Process Mesh Batch");
        for (auto& meshData : meshBatch) {
            const glm::ivec3 chunkCoords = meshData.chunkPosition;

            // Check if chunk is still within acceptable distance
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

    if (!meshBatch.empty()) {
        ZoneScopedN("Submit Batched Uploads");
        _meshPool->submitPendingUploads([this](std::function<void(VkCommandBuffer)>&& func) {
            _executor.immediateSubmit(std::move(func));
        });
    }

    const auto now = std::chrono::steady_clock::now();
    const auto msSinceLastUpload =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastUploadTime).count();

    const bool shouldUpload = _chunkDataDirty && _useMeshShaders &&
                              (_dirtyChunkCount >= _adaptiveMinChunks ||
                               msSinceLastUpload >= _adaptiveMaxMs);

    if (shouldUpload) {
        ZoneScopedN("Upload Chunk Data (GPU-Persistent)");

        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);

        ensureBufferCapacity(static_cast<uint32_t>(_chunkDrawInfos.size()));

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
            if (!_dirtyChunkList.empty() && _dirtyChunkList.size() < _chunkDrawData.size()) {
                ZoneScopedN("Upload Dirty Chunks Only");

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
            } else {
                ZoneScopedN("Upload All Chunks");
                _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                              _chunkDrawData.size() * sizeof(GPUChunkData));
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

    const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
    _meshPool->setCurrentFrameIndex(frameIndex);

    // Process deletion queue at the start of each frame
    // Buffers queued for deletion are now safe to destroy
    {
        ZoneScopedN("Process Deletion Queue");
        _meshPool->processDeletionQueue();
    }

    if (_indexBufferResizePending) {
        ZoneScopedN("Process Index Buffer Resize");

        vkDeviceWaitIdle(_device.getDevice());

        for (uint32_t i = 0; i < CHUNK_BUFFER_COUNT; ++i) {
            DescriptorWriter writer;
            writer.writeBuffer(2, _meshPool->getIndexBuffer(),
                              _meshPool->getIndexBufferCapacity(), 0,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(_device.getDevice(), _meshShaderDescriptorSets[i]);
        }

        _indexBufferResizePending = false;
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
            chunkData.firstIndex = buffers.firstIndex;
            chunkData._padding = 0;
            _chunkDrawData.push_back(chunkData);
        }
    }

    // Early exit if nothing to draw
    if (_chunkDrawData.empty()) {
        return;
    }

    ensureBufferCapacity(static_cast<uint32_t>(_chunkDrawData.size()));

    {
        ZoneScopedN("Upload Chunk Data");
        const uint32_t frameIndex = static_cast<uint32_t>(_renderer.getFrameNumber() % CHUNK_BUFFER_COUNT);
        _bufferManager.uploadToBuffer(_chunkDataBuffers[frameIndex], _chunkDrawData.data(),
                                      _chunkDrawData.size() * sizeof(GPUChunkData));
    }

    // GPU frustum culling compute pass
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

        _cullingStats.totalChunks = pushConstants.totalChunks;
        _cullingStats.visibleChunks = 0;
        _cullingStats.culledChunks = 0;
        _cullingStats.cullingPercentage = 0.0f;
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

    // Mesh shader path vs traditional path
    if (_useMeshShaders) {
        ZoneScopedN("Mesh Shader Rendering");
        TracyVkZone(_device.getTracyCtx(), cmd, "GPU Mesh Shader Rendering");

        // Bind mesh shader pipeline (wireframe or filled based on F1 toggle)
        VkPipeline activePipeline = wireframeMode ? _meshShaderWireframePipeline.getPipeline()
                                                  : _meshShaderPipeline.getPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

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
        pushConstants.maxVerticesPerMeshWorkgroup = 256;
        pushConstants.maxMeshWorkgroupsPerTask = _maxMeshWorkgroupsPerTask;

        vkCmdPushConstants(cmd, _meshShaderPipelineLayout, VK_SHADER_STAGE_TASK_BIT_EXT, 0,
                           sizeof(MeshShaderPushConstants), &pushConstants);

        {
            ZoneScopedN("Dispatch Mesh Tasks");
            uint32_t taskWorkgroups = (pushConstants.totalChunks + 15) / 16;

            // Safety bounds checking and logging
            const uint32_t MAX_SAFE_TASK_WORKGROUPS = 512;
            if (taskWorkgroups > MAX_SAFE_TASK_WORKGROUPS) {
                std::cerr << "[VoxelRenderer] WARNING: taskWorkgroups=" << taskWorkgroups
                          << " exceeds safe limit (" << MAX_SAFE_TASK_WORKGROUPS
                          << "), clamping to prevent crash\n";
                taskWorkgroups = MAX_SAFE_TASK_WORKGROUPS;
            }

            std::cout << "[VoxelRenderer] Dispatching " << taskWorkgroups << " task workgroups for "
                      << pushConstants.totalChunks << " chunks\n";

            // Add debug label to isolate crash location
            PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginLabel =
                (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetDeviceProcAddr(
                    _device.getDevice(), "vkCmdBeginDebugUtilsLabelEXT");
            PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndLabel =
                (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetDeviceProcAddr(_device.getDevice(),
                                                                    "vkCmdEndDebugUtilsLabelEXT");

            if (vkCmdBeginLabel) {
                VkDebugUtilsLabelEXT label{};
                label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
                label.pLabelName = "Mesh Shader Draw";
                vkCmdBeginLabel(cmd, &label);
            }

            _vkCmdDrawMeshTasksEXT(cmd, taskWorkgroups, 1, 1);

            if (vkCmdEndLabel) {
                vkCmdEndLabel(cmd);
            }
        }

    } else {
        // Traditional vertex/fragment shader path
        ZoneScopedN("Traditional Rendering");
        TracyVkZone(_device.getTracyCtx(), cmd, "GPU Traditional Rendering");

        // Bind pipeline based on wireframe mode
        VkPipeline activePipeline =
            wireframeMode ? _voxelWireframePipeline.getPipeline() : _voxelPipeline.getPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

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

    // VMA per-chunk buffers allow instant unload

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

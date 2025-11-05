#include "VoxelRenderer.hpp"

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
                             ThreadSafeQueue<MeshData>& finishedMeshQueue)
    : _device(device), _meshManager(meshManager), _blockRegistry(registry), _context(context),
      _executor(executor), _bufferManager(bufferManager), _descriptorAllocator(descriptorAllocator),
      _renderer(renderer), _finishedMeshQueue(finishedMeshQueue) {
    // Initialize mesh buffer pool
    _meshPool = std::make_unique<MeshBufferPool>(_device, _bufferManager);
}

VoxelRenderer::~VoxelRenderer() {
    auto startTime = std::chrono::high_resolution_clock::now();
    std::cout << "[VoxelRenderer] Destructor start - " << _chunkDrawInfos.size()
              << " chunks loaded\n";

    // CRITICAL: Free all chunk vertex buffers BEFORE clearing vectors!
    // VMA requires explicit deallocation before the allocator is destroyed.
    // Extract mesh buffers into temp vector for batch destruction (fast!)
    std::vector<ChunkMeshBuffers> allBuffers;
    allBuffers.reserve(_chunkDrawInfos.size());
    for (const auto& info : _chunkDrawInfos) {
        allBuffers.push_back(info.meshBuffers);
    }

    // Batch destroy all vertex buffers (fast ~70ms for 15k chunks)
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
    if (_frustumCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _frustumCullPipelineLayout, nullptr);
        _frustumCullPipelineLayout = VK_NULL_HANDLE;
    }

    // Clean up descriptor set layouts
    if (_chunkSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _chunkSetLayout, nullptr);
    }
    if (_frustumCullSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _frustumCullSetLayout, nullptr);
    }

    // Clean up MDI buffers
    if (_indirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indirectBuffer);
    }
    if (_chunkDataBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_chunkDataBuffer);
    }
    if (_atlasConfigBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_atlasConfigBuffer);
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

    VkShaderModule voxelFragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");
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

    // Create buffer for per-chunk data (SSBO)
    _chunkDataBuffer = _bufferManager.createBuffer(sizeof(GPUChunkData) * _currentMaxChunks,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Create uniform buffer for atlas configuration
    _atlasConfigBuffer = _bufferManager.createBuffer(
        sizeof(int), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Upload texturesPerRow to the uniform buffer
    _bufferManager.uploadToBuffer(_atlasConfigBuffer, &texturesPerRow, sizeof(int));

    // Allocate descriptor set for chunk data SSBO
    _chunkDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _chunkSetLayout, nullptr);

    // Write descriptor set to bind the chunk data buffer, texture atlas, and atlas config
    DescriptorWriter writer;
    writer.writeBuffer(0, _chunkDataBuffer.buffer, sizeof(GPUChunkData) * _currentMaxChunks, 0,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.writeBuffer(2, _atlasConfigBuffer.buffer, sizeof(int), 0,
                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.updateSet(_device.getDevice(), _chunkDescriptorSet);
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
    // Binding 0: Input chunk data (will be reused from main rendering pipeline)
    computeWriter.writeBuffer(0, _chunkDataBuffer.buffer, sizeof(GPUChunkData) * _currentMaxChunks,
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
    // _chunkDescriptorSet For now, we'll update them separately - TODO: optimize by reusing
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
    _bufferManager.destroyBuffer(_chunkDataBuffer);

    // Create new larger buffers
    _indirectBuffer = _bufferManager.createBuffer(
        sizeof(VkDrawIndexedIndirectCommand) * newCapacity,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    _chunkDataBuffer = _bufferManager.createBuffer(sizeof(GPUChunkData) * newCapacity,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Update descriptor set to bind the new chunk data buffer
    // Only update binding 0 (chunk data SSBO), keep bindings 1 and 2 unchanged
    DescriptorWriter writer;
    writer.writeBuffer(0, _chunkDataBuffer.buffer, sizeof(GPUChunkData) * newCapacity, 0,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(_device.getDevice(), _chunkDescriptorSet);

    // Update current capacity
    _currentMaxChunks = newCapacity;
}

void VoxelRenderer::update(const glm::ivec3& cameraChunkPos, int maxLoadDistance) {
    ZoneScoped;
    // Process all available finished meshes without blocking
    while (auto meshOpt = _finishedMeshQueue.try_pop()) {
        MeshData meshData = std::move(meshOpt.value());

        const glm::ivec3 chunkCoords = meshData.chunkPosition;

        // CRITICAL: Check if chunk is still within acceptable distance
        // If player has moved away, discard this mesh to prevent "ghost chunks"
        const glm::ivec3 offset = chunkCoords - cameraChunkPos;
        const int chebyshevDistance =
            std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

        if (chebyshevDistance > maxLoadDistance) {
            // Chunk is too far away - discard this mesh
            // This prevents loading chunks that finished meshing after the player moved away
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
            }
            continue;
        }

        // Allocate dedicated buffers for this chunk via VMA
        ChunkMeshBuffers chunkBuffers =
            _meshPool->allocateChunkBuffers(meshData.indices, meshData.vertices,
                                            [this](std::function<void(VkCommandBuffer)>&& func) {
                                                _executor.immediateSubmit(std::move(func));
                                            });

        const glm::vec3 chunkWorldPos{static_cast<float>(chunkCoords[0] * Chunk::CHUNK_SIZE),
                                      static_cast<float>(chunkCoords[1] * Chunk::CHUNK_SIZE),
                                      static_cast<float>(chunkCoords[2] * Chunk::CHUNK_SIZE)};

        if (auto it = _chunkDrawLookup.find(chunkCoords); it != _chunkDrawLookup.end()) {
            // Chunk is being updated - free old buffers first
            ChunkDrawInfo& info = _chunkDrawInfos[it->second];
            _meshPool->freeChunkBuffers(info.meshBuffers);

            info.chunkCoords = chunkCoords;
            info.worldPosition = chunkWorldPos;
            info.meshBuffers = chunkBuffers;
        } else {
            const size_t index = _chunkDrawInfos.size();
            _chunkDrawInfos.push_back(ChunkDrawInfo{.chunkCoords = chunkCoords,
                                                    .worldPosition = chunkWorldPos,
                                                    .meshBuffers = chunkBuffers});
            _chunkDrawLookup.emplace(chunkCoords, index);
        }
    }
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {
    // Process deletion queue at the start of each frame
    // Buffers queued for deletion are now safe to destroy
    _meshPool->processDeletionQueue();

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();
    VkExtent2D drawExtent = _context.getDrawExtent();

    // Build chunk data (needed for both CPU and GPU culling paths)
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

    // Early exit if nothing to draw
    if (_chunkDrawData.empty()) {
        return;
    }

    // Ensure buffers are large enough
    ensureBufferCapacity(static_cast<uint32_t>(_chunkDrawData.size()));

    // Upload chunk data to GPU (input for compute shader)
    {
        ZoneScopedN("Upload Chunk Data");
        _bufferManager.uploadToBuffer(_chunkDataBuffer, _chunkDrawData.data(),
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
        // Max render distance: 24 chunks * 32m = 768m (matches unload radius of 20 chunks + buffer)
        frustumData.maxRenderDistance = 768.0F;

        {
            ZoneScopedN("Upload Frustum Data");
            _bufferManager.uploadToBuffer(_frustumUniformBuffer, &frustumData,
                                          sizeof(GPUFrustumData));
        }

        // 2. Reset atomic counter (drawCount = 0) using GPU command
        // vkCmdFillBuffer is faster than CPU upload (no staging buffer needed)
        vkCmdFillBuffer(cmd, _culledIndirectBuffer.buffer, 0, sizeof(uint32_t), 0);

        // Memory barrier: ensure fill completes before compute reads/writes
        VkMemoryBarrier fillBarrier{};
        fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fillBarrier, 0, nullptr,
                             0, nullptr);

        // 3. Bind compute pipeline and descriptor set
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _frustumCullPipeline.getPipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _frustumCullPipelineLayout, 0,
                                1, &_frustumCullDescriptorSet, 0, nullptr);

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

    // Bind pipeline based on wireframe mode
    {
        ZoneScopedN("Bind Graphics Pipeline");
        VkPipeline activePipeline =
            wireframeMode ? _voxelWireframePipeline.getPipeline() : _voxelPipeline.getPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);
    }

    // Bind descriptor set for chunk data SSBO
    // When GPU culling is enabled, bind the culled chunk data buffer; otherwise bind the original
    VkDescriptorSet activeChunkSet =
        _enableGPUCulling ? _culledChunkDescriptorSet : _chunkDescriptorSet;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _voxelPipeline.getLayout(), 0, 1,
                            &activeChunkSet, 0, nullptr);

    // Set up view-projection matrix
    glm::mat4 view = camera.getViewMatrix();

    // Standard perspective projection for Vulkan
    glm::mat4 projection = glm::perspective(
        glm::radians(80.0F),
        static_cast<float>(drawExtent.width) / static_cast<float>(drawExtent.height),
        0.1F,    // near plane
        10000.0F // far plane
    );
    projection[1][1] *= -1.0F; // Flip Y for Vulkan coordinate system

    glm::mat4 viewProjection = projection * view;

    // Push constants now only contain GLOBAL data (view-projection matrix)
    vkCmdPushConstants(cmd, _voxelPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(glm::mat4), &viewProjection);

    // ========================================================================
    // BIND MEGA INDEX BUFFER (shared by all chunks)
    // ========================================================================
    // Vertex data is accessed per-chunk via buffer device address in shader
    // Index data is in one mega buffer, accessed via firstIndex offset
    VkBuffer indexBuffer = _meshPool->getIndexBuffer();
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Multi-Draw Indirect - choose best available method
    {
        ZoneScopedN("Indirect Draw Calls");
        if (_enableGPUCulling && _supportsDrawIndirectCount) {
            // OPTIMAL: GPU culling with GPU-driven draw count
            // drawCount is read from buffer offset 0, commands start at offset sizeof(uint32_t)
            vkCmdDrawIndexedIndirectCount(
                cmd, _culledIndirectBuffer.buffer,
                sizeof(uint32_t), // offset to first command (skip drawCount)
                _culledIndirectBuffer.buffer,
                0,                                            // offset to drawCount
                static_cast<uint32_t>(_chunkDrawData.size()), // max draws
                sizeof(VkDrawIndexedIndirectCommand));
        } else if (_enableGPUCulling) {
            // FALLBACK: GPU culling but CPU-specified max count
            // Still benefits from GPU culling, but GPU may process empty draw commands
            // ~5% slower than optimal but works on all devices
            vkCmdDrawIndexedIndirect(cmd, _culledIndirectBuffer.buffer,
                                     sizeof(uint32_t), // offset to first command
                                     static_cast<uint32_t>(_chunkDrawData.size()), // max draws
                                     sizeof(VkDrawIndexedIndirectCommand));
        } else {
            // CPU PATH: traditional indirect draw (for debugging)
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
        const ChunkDrawInfo& info = _chunkDrawInfos[idx];

        // Free the chunk's GPU buffers - INSTANT! No GPU stall!
        _meshPool->freeChunkBuffers(info.meshBuffers);

        // Remove from tracking (swap-and-pop)
        if (idx != _chunkDrawInfos.size() - 1) {
            _chunkDrawInfos[idx] = _chunkDrawInfos.back();
            _chunkDrawLookup[_chunkDrawInfos[idx].chunkCoords] = idx;
        }
        _chunkDrawInfos.pop_back();
        _chunkDrawLookup.erase(it);
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

#include "VoxelRenderer.hpp"

#include <map>
#include <stdexcept>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../../Game/Camera.hpp"
#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
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
    _voxelPipeline.cleanup(_device);
    _voxelWireframePipeline.cleanup(_device);
    // Clean up owned pipeline layout
    if (_voxelPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _voxelPipelineLayout, nullptr);
        _voxelPipelineLayout = VK_NULL_HANDLE;
    }

    // Clean up descriptor set layout
    if (_chunkSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _chunkSetLayout, nullptr);
    }

    // Clean up MDI buffers
    if (_indirectBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_indirectBuffer);
    }
    if (_chunkDataBuffer.buffer != VK_NULL_HANDLE) {
        _bufferManager.destroyBuffer(_chunkDataBuffer);
    }
}

void VoxelRenderer::initPipelines(VkImageView atlasView, VkSampler atlasSampler) {
    // First initialize MDI resources and descriptor set layout
    initMDI(atlasView, atlasSampler);

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

    VkVertexInputBindingDescription binding{
        .binding = 0, .stride = sizeof(uint32_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

    // A single attribute: the packed uint32_t itself
    std::vector<VkVertexInputAttributeDescription> attributes{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 0}};

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
    pipelineBuilder.setVertexInputState({binding}, attributes);

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
    pipelineBuilder.setVertexInputState({binding}, attributes);

    VkPipeline voxelWireframePipeline = pipelineBuilder.build(_device.getDevice());
    _voxelWireframePipeline.init(voxelWireframePipeline, _voxelPipelineLayout);

    vkDestroyShaderModule(_device.getDevice(), voxelFragShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), voxelVertexShader, nullptr);
}

void VoxelRenderer::initMDI(VkImageView atlasView, VkSampler atlasSampler) {
    // Create descriptor set layout for chunk data SSBO and texture atlas
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    layoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    _chunkSetLayout = layoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_VERTEX_BIT |
                                                                   VK_SHADER_STAGE_FRAGMENT_BIT);

    // Create buffers for indirect draw commands
    // Size for max 10000 chunks
    constexpr uint32_t MAX_CHUNKS = 10000;
    _indirectBuffer = _bufferManager.createBuffer(sizeof(VkDrawIndexedIndirectCommand) * MAX_CHUNKS,
                                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Create buffer for per-chunk data (SSBO)
    _chunkDataBuffer = _bufferManager.createBuffer(sizeof(GPUChunkData) * MAX_CHUNKS,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Allocate descriptor set for chunk data SSBO
    _chunkDescriptorSet =
        _descriptorAllocator.allocate(_device.getDevice(), _chunkSetLayout, nullptr);

    // Write descriptor set to bind the chunk data buffer AND texture atlas
    DescriptorWriter writer;
    writer.writeBuffer(0, _chunkDataBuffer.buffer, sizeof(GPUChunkData) * MAX_CHUNKS, 0,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeImage(1, atlasView, atlasSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.updateSet(_device.getDevice(), _chunkDescriptorSet);
}

void VoxelRenderer::update() {
    // Process all available finished meshes without blocking
    while (auto meshOpt = _finishedMeshQueue.try_pop()) {
        MeshData meshData = std::move(meshOpt.value());

        if (meshData.vertices.empty() || meshData.indices.empty()) {
            continue;
        }

        // Upload mesh to GPU (fast operation)
        MeshAllocation allocation =
            _meshPool->uploadMesh(meshData.indices, meshData.vertices,
                                  [this](std::function<void(VkCommandBuffer)>&& func) {
                                      _executor.immediateSubmit(std::move(func));
                                  });

        const glm::ivec3 chunkCoords = meshData.chunkPosition;
        const glm::vec3 chunkWorldPos{static_cast<float>(chunkCoords[0] * Chunk::CHUNK_SIZE),
                                      static_cast<float>(chunkCoords[1] * Chunk::CHUNK_SIZE),
                                      static_cast<float>(chunkCoords[2] * Chunk::CHUNK_SIZE)};

        if (auto it = _chunkDrawLookup.find(chunkCoords); it != _chunkDrawLookup.end()) {
            ChunkDrawInfo& info = _chunkDrawInfos[it->second];
            info.chunkCoords = chunkCoords;
            info.worldPosition = chunkWorldPos;
            info.mesh = allocation;
        } else {
            const size_t index = _chunkDrawInfos.size();
            _chunkDrawInfos.push_back(ChunkDrawInfo{
                .chunkCoords = chunkCoords, .worldPosition = chunkWorldPos, .mesh = allocation});
            _chunkDrawLookup.emplace(chunkCoords, index);
        }
    }
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();
    VkExtent2D drawExtent = _context.getDrawExtent();

    _indirectCommands.clear();
    _chunkDrawData.clear();

    _indirectCommands.reserve(_chunkDrawInfos.size());
    _chunkDrawData.reserve(_chunkDrawInfos.size());

    for (const ChunkDrawInfo& drawInfo : _chunkDrawInfos) {
        const MeshAllocation& mesh = drawInfo.mesh;
        if (mesh.indexCount == 0) {
            continue;
        }

        VkDrawIndexedIndirectCommand indirectCmd{};
        indirectCmd.indexCount = mesh.indexCount;
        indirectCmd.instanceCount = 1;
        indirectCmd.firstIndex = mesh.firstIndex;
        indirectCmd.vertexOffset = mesh.vertexOffset;
        indirectCmd.firstInstance = 0;
        _indirectCommands.push_back(indirectCmd);

        GPUChunkData chunkData{};
        chunkData.chunkWorldPos = drawInfo.worldPosition;
        chunkData.padding = 0.0F;
        _chunkDrawData.push_back(chunkData);
    }

    // Early exit if nothing to draw
    if (_indirectCommands.empty()) {
        return;
    }

    // Upload data to GPU buffers
    _bufferManager.uploadToBuffer(_indirectBuffer, _indirectCommands.data(),
                                  _indirectCommands.size() * sizeof(VkDrawIndexedIndirectCommand));
    _bufferManager.uploadToBuffer(_chunkDataBuffer, _chunkDrawData.data(),
                                  _chunkDrawData.size() * sizeof(GPUChunkData));

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
    VkPipeline activePipeline =
        wireframeMode ? _voxelWireframePipeline.getPipeline() : _voxelPipeline.getPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    // Bind descriptor set for chunk data SSBO
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _voxelPipeline.getLayout(), 0, 1,
                            &_chunkDescriptorSet, 0, nullptr);

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

    // These contain ALL chunk mesh data, indexed by the indirect commands
    VkBuffer vertexBuffer = _meshPool->getVertexBuffer();
    VkBuffer indexBuffer = _meshPool->getIndexBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Multi-Draw Indirect
    vkCmdDrawIndexedIndirect(cmd, _indirectBuffer.buffer, 0,
                             static_cast<uint32_t>(_indirectCommands.size()),
                             sizeof(VkDrawIndexedIndirectCommand));

    vkCmdEndRendering(cmd);
}

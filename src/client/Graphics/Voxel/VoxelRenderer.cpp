#include "VoxelRenderer.hpp"

#include <map>
#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../../Game/Camera.hpp"
#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
#include "../Pipeline/GraphicsPipelineBuilder.hpp"
#include "../Rendering/CommandExecutor.hpp"
#include "../Rendering/RenderContext.hpp"
#include "common/World/Chunk.hpp"
#include "common/World/ChunkMesh.hpp"
#include "MeshBufferPool.hpp"
#include "MeshManager.hpp"

VoxelRenderer::VoxelRenderer(VulkanDevice& device, MeshManager& meshManager,
                             BlockRegistry& registry, RenderContext& context,
                             CommandExecutor& executor, VulkanBuffer& bufferManager,
                             DescriptorAllocatorGrowable& descriptorAllocator)
    : _device(device), _meshManager(meshManager), _blockRegistry(registry), _context(context),
      _executor(executor), _bufferManager(bufferManager),
      _descriptorAllocator(descriptorAllocator) {
    // Initialize mesh buffer pool
    _meshPool = std::make_unique<MeshBufferPool>(_device, _bufferManager);
}

VoxelRenderer::~VoxelRenderer() {
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

void VoxelRenderer::initPipelines() {
    // First initialize MDI resources and descriptor set layout
    initMDI();

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

    VkPipelineLayout voxelPipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutInfo, nullptr,
                               &voxelPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create voxel pipeline layout");
    }

    // --- PACKED VERTEX FORMAT ---
    // A single binding for our packed uint32_t vertex data
    VkVertexInputBindingDescription binding{
        .binding = 0, .stride = sizeof(uint32_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

    // A single attribute: the packed uint32_t itself
    std::vector<VkVertexInputAttributeDescription> attributes{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 0}};

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();

    // Create FILLED pipeline
    GraphicsPipelineBuilder pipelineBuilder;
    pipelineBuilder.setPipelineLayout(voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(drawImage.format);
    pipelineBuilder.setDepthFormat(depthImage.format);
    pipelineBuilder.setVertexInputState({binding}, attributes);

    VkPipeline voxelPipeline = pipelineBuilder.build(_device.getDevice());
    _voxelPipeline.init(voxelPipeline, voxelPipelineLayout);

    // Create WIREFRAME pipeline
    pipelineBuilder.clear();
    pipelineBuilder.setPipelineLayout(voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_LINE); // WIREFRAME MODE
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(drawImage.format);
    pipelineBuilder.setDepthFormat(depthImage.format);
    pipelineBuilder.setVertexInputState({binding}, attributes);

    VkPipeline voxelWireframePipeline = pipelineBuilder.build(_device.getDevice());
    _voxelWireframePipeline.init(voxelWireframePipeline, voxelPipelineLayout);

    vkDestroyShaderModule(_device.getDevice(), voxelFragShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), voxelVertexShader, nullptr);
}

void VoxelRenderer::initMDI() {
    // Create descriptor set layout for chunk data SSBO
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    _chunkSetLayout = layoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_VERTEX_BIT);

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

    // Write descriptor set to bind the chunk data buffer
    DescriptorWriter writer;
    writer.writeBuffer(0, _chunkDataBuffer.buffer, sizeof(GPUChunkData) * MAX_CHUNKS, 0,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(_device.getDevice(), _chunkDescriptorSet);
}

void VoxelRenderer::initTestChunk() {
    // Simple struct to use as a key in our chunk map
    struct ChunkPos {
        int x, y, z;
        bool operator<(const ChunkPos& other) const {
            if (x != other.x) {
                return x < other.x;
            }
            if (y != other.y) {
                return y < other.y;
            }
            return z < other.z;
        }
    };

    // --- PART 1: Create all chunk data first ---
    std::map<ChunkPos, std::unique_ptr<Chunk>> worldChunks;

    _chunkPositions.clear();
    const int renderDistance = 32; // Create a 64x64 grid of chunks
    for (int x = -renderDistance; x < renderDistance; ++x) {
        for (int z = -renderDistance; z < renderDistance; ++z) {
            // All chunks are at y=0 for this test
            ChunkPos pos = {x, 0, z};
            _chunkPositions.push_back(
                glm::vec3(x * Chunk::CHUNK_SIZE, 0.0F, z * Chunk::CHUNK_SIZE));

            // Create a new chunk and generate its block data
            auto chunk = std::make_unique<Chunk>();

            // Fill with some blocks for testing (staircase-like pattern)
            for (int bx = 0; bx < 32; bx++) {
                for (int bz = 0; bz < 32; bz++) {
                    int height = (bx + bz) / 2;
                    for (int by = 0; by < height && by < 32; by++) {
                        if (by < height - 5) {
                            chunk->setBlock(bx, by, bz, 1); // stone
                        } else if (by < height - 1) {
                            chunk->setBlock(bx, by, bz, 2); // grass_block
                        } else if (by < height && (bx % 3 == 0) && (bz % 3 == 0)) {
                            chunk->setBlock(bx, by, bz, 4); // water
                        } else if (by < height) {
                            chunk->setBlock(bx, by, bz, 3); // oak_wood (some trees)
                        } else {
                            chunk->setBlock(bx, by, bz, 0); // air
                        }
                    }
                }
            }
            worldChunks[pos] = std::move(chunk);
        }
    }

    // --- PART 2: Generate meshes for all chunks, now with neighbor data ---
    _meshPool->reset(); // Reset the pool before generating new meshes

    for (const auto& [pos, chunk] : worldChunks) {
        // Find the 6 neighbors for the current chunk
        auto findNeighbor = [&](int dx, int dy, int dz) -> const Chunk* {
            auto it = worldChunks.find({pos.x + dx, pos.y + dy, pos.z + dz});
            return (it != worldChunks.end()) ? it->second.get() : nullptr;
        };

        const Chunk* neighborNorth = findNeighbor(0, 0, 1);
        const Chunk* neighborSouth = findNeighbor(0, 0, -1);
        const Chunk* neighborEast = findNeighbor(1, 0, 0);
        const Chunk* neighborWest = findNeighbor(-1, 0, 0);
        const Chunk* neighborTop = findNeighbor(0, 1, 0);     // Always null in our test grid
        const Chunk* neighborBottom = findNeighbor(0, -1, 0); // Always null in our test grid

        // Generate the mesh for this specific chunk with neighbor awareness
        std::vector<VoxelVertex> vertices;
        std::vector<uint32_t> indices;
        ChunkMesh::generateMesh(*chunk, _blockRegistry, vertices, indices, neighborNorth,
                                neighborSouth, neighborEast, neighborWest, neighborTop,
                                neighborBottom);

        if (vertices.empty() || indices.empty()) {
            continue; // Skip empty meshes
        }

        // Upload this chunk's mesh to the pool
        // For this test, since all chunks have identical geometry, we only upload once
        if (_sharedChunkMeshAllocation.indexCount == 0) {
            _sharedChunkMeshAllocation = _meshPool->uploadMesh(
                indices, vertices, [this](std::function<void(VkCommandBuffer)>&& func) {
                    _executor.immediateSubmit(std::move(func));
                });
        }
    }

    // Safety check in case all meshes were empty
    if (_sharedChunkMeshAllocation.indexCount == 0) {
        throw std::runtime_error("Failed to generate chunk mesh: no vertices or indices");
    }
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {
    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();
    VkExtent2D drawExtent = _context.getDrawExtent();

    // --- PREPARE MDI DATA ON CPU ---
    _indirectCommands.clear();
    _chunkDrawData.clear();

    _indirectCommands.reserve(_chunkPositions.size());
    _chunkDrawData.reserve(_chunkPositions.size());

    // Iterate over all chunk positions and build the command list
    // All chunks share the same mesh geometry, but render at different positions
    for (size_t i = 0; i < _chunkPositions.size(); ++i) {
        // Create the indirect command. The geometry data is the same for all.
        VkDrawIndexedIndirectCommand indirectCmd{};
        indirectCmd.indexCount = _sharedChunkMeshAllocation.indexCount;
        indirectCmd.instanceCount = 1; // Always 1 instance per draw command in MDI
        indirectCmd.firstIndex = _sharedChunkMeshAllocation.firstIndex;
        indirectCmd.vertexOffset = _sharedChunkMeshAllocation.vertexOffset;
        indirectCmd.firstInstance = 0; // Set to 0 because we use gl_DrawID in the shader
        _indirectCommands.push_back(indirectCmd);

        // Create the per-chunk data (its world position)
        GPUChunkData chunkData{};
        chunkData.chunkWorldPos = _chunkPositions[i];
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

    // --- BEGIN RENDERING ---
    VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = drawImage.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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

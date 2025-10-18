#include "VoxelRenderer.hpp"

#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../../Game/Camera.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Pipeline/GraphicsPipelineBuilder.hpp"
#include "../Rendering/CommandExecutor.hpp"
#include "../Rendering/RenderContext.hpp"
#include "common/World/Chunk.hpp"
#include "common/World/ChunkMesh.hpp"
#include "MeshManager.hpp"

VoxelRenderer::VoxelRenderer(VulkanDevice& device, MeshManager& meshManager,
                             BlockRegistry& registry, RenderContext& context,
                             CommandExecutor& executor)
    : _device(device), _meshManager(meshManager), _blockRegistry(registry), _context(context),
      _executor(executor) {}

VoxelRenderer::~VoxelRenderer() = default;

void VoxelRenderer::initPipelines() {
    VkShaderModule voxelFragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");
    VkShaderModule voxelVertexShader =
        Pipeline::loadShaderModule(_device, "shaders/voxel.vert.spv");

    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(ChunkPushConstants)};

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .pNext = nullptr,
                                                  .flags = 0,
                                                  .setLayoutCount = 0,
                                                  .pSetLayouts = nullptr,
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

void VoxelRenderer::initTestChunk() {
    _testChunk = std::make_unique<Chunk>();

    // Fill with some blocks for testing (staircase-like pattern)
    for (int x = 0; x < 32; x++) {
        for (int z = 0; z < 32; z++) {
            int height = (x + z) / 2;
            for (int y = 0; y < height && y < 32; y++) {
                if (y < height - 5) {
                    _testChunk->setBlock(x, y, z, 1); // stone
                } else if (y < height - 1) {
                    _testChunk->setBlock(x, y, z, 2); // grass_block
                } else if (y < height && (x % 3 == 0) && (z % 3 == 0)) {
                    _testChunk->setBlock(x, y, z, 4); // water
                } else if (y < height) {
                    _testChunk->setBlock(x, y, z, 3); // oak_wood (some trees)
                } else {
                    _testChunk->setBlock(x, y, z, 0); // air
                }
            }
        }
    }

    // Generate mesh for the chunk
    std::vector<VoxelVertex> vertices;
    std::vector<uint32_t> indices;

    ChunkMesh::generateMesh(*_testChunk, _blockRegistry, vertices, indices);

    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("Failed to generate chunk mesh: no vertices or indices");
    }

    // Upload mesh to GPU using immediate submit
    // VoxelVertex is now uint32_t, so we can pass directly
    _testChunkMesh = _meshManager.uploadMesh(indices, vertices,
                                             [this](std::function<void(VkCommandBuffer)>&& func) {
                                                 _executor.immediateSubmit(std::move(func));
                                             });
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {
    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();
    VkExtent2D drawExtent = _context.getDrawExtent();

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

    // Set push constants
    ChunkPushConstants pushConstants{};
    pushConstants.viewProjection = viewProjection;
    pushConstants.chunkWorldPos = glm::vec3(0.0F, 0.0F, 0.0F);

    vkCmdPushConstants(cmd, _voxelPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(ChunkPushConstants), &pushConstants);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {_testChunkMesh.vertexBuffer.buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, _testChunkMesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Get index count from buffer size
    auto indexCount =
        static_cast<uint32_t>(_testChunkMesh.indexBuffer.info.size / sizeof(uint32_t));

    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);

    vkCmdEndRendering(cmd);
}

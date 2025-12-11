#include "VoxelDrawDispatcher.hpp"

#include <array>

#include <tracy/Tracy.hpp>

#include "../../Game/Camera.hpp"
#include "../GraphicsUtils.hpp"
#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Renderer.hpp"
#include "../Rendering/RenderContext.hpp"
#include "ChunkBufferManager.hpp"
#include "VoxelPipelineManager.hpp"

VoxelDrawDispatcher::VoxelDrawDispatcher(VulkanDevice& device, VulkanBuffer& bufferManager,
                                         RenderContext& context, Renderer& renderer,
                                         VoxelPipelineManager& pipelineManager,
                                         ChunkBufferManager& bufferMgr)
    : _device(device), _bufferManager(bufferManager), _context(context), _renderer(renderer),
      _pipelineManager(pipelineManager), _bufferMgr(bufferMgr) {}

void VoxelDrawDispatcher::draw(VkCommandBuffer cmd, Camera& camera, bool wireframeMode,
                               bool enableGPUCulling) {
    ZoneScopedN("VoxelDrawDispatcher::draw");

    const uint32_t frameIndex = static_cast<uint32_t>(
        _renderer.getFrameNumber() % ChunkBufferManager::CHUNK_BUFFER_COUNT);

    const auto& chunkDrawData = _bufferMgr.getChunkDrawData();

    // Build chunk data for traditional path (mesh shader path builds in update())
    if (!_pipelineManager.supportsMeshShaders()) {
        ZoneScopedN("Build Chunk Data (Traditional)");
        _bufferMgr.buildIndirectCommands();
    }

    // Early exit if nothing to draw
    if (chunkDrawData.empty()) {
        return;
    }

    _bufferMgr.ensureBufferCapacity(static_cast<uint32_t>(chunkDrawData.size()));

    {
        ZoneScopedN("Upload Chunk Data");
        _bufferMgr.uploadChunkData(frameIndex);
    }

    // GPU frustum culling
    if (enableGPUCulling) {
        dispatchGPUCulling(cmd, camera, _bufferMgr.getMaxLoadDistance());
    } else if (!_pipelineManager.supportsMeshShaders()) {
        buildCPUIndirectCommands(cmd);
    }

    // Setup rendering
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
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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

    if (_pipelineManager.supportsMeshShaders()) {
        uploadCameraData(cmd, camera);
    }

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

    if (_pipelineManager.supportsMeshShaders()) {
        drawMeshShaderPath(cmd, wireframeMode);
    } else {
        drawTraditionalPath(cmd, camera, wireframeMode, enableGPUCulling);
    }

    vkCmdEndRendering(cmd);
}

void VoxelDrawDispatcher::uploadCameraData(VkCommandBuffer cmd, Camera& camera) {
    ZoneScopedN("Upload Mesh Shader Camera Data");

    VkExtent2D drawExtent = _context.getDrawExtent();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 projection =
        GraphicsUtils::createVulkanProjectionFromExtent(drawExtent.width, drawExtent.height);

    GPUCameraData cameraData{};
    cameraData.viewProjection = projection * view;

    std::array<glm::vec4, 6> frustumPlanes = camera.getFrustumPlanes(projection);
    for (size_t i = 0; i < 6; ++i) {
        cameraData.planes[i] = frustumPlanes[i];
    }
    cameraData.cameraPos = camera.getPosition();
    cameraData.maxRenderDistance =
        GraphicsUtils::calculateRenderDistance(_bufferMgr.getMaxLoadDistance());

    {
        ZoneScopedN("Upload Camera UBO");
        _bufferManager.uploadToBuffer(_bufferMgr.getCameraUniformBuffer(), &cameraData,
                                      sizeof(GPUCameraData));
    }

    // Memory barrier
    VkMemoryBarrier uploadBarrier{};
    uploadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    uploadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT |
                             VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
                         0, 1, &uploadBarrier, 0, nullptr, 0, nullptr);
}

void VoxelDrawDispatcher::dispatchGPUCulling(VkCommandBuffer cmd, Camera& camera,
                                             int maxLoadDistance) {
    ZoneScopedN("GPU Frustum Culling");

    VkExtent2D drawExtent = _context.getDrawExtent();
    glm::mat4 projection =
        GraphicsUtils::createVulkanProjectionFromExtent(drawExtent.width, drawExtent.height);
    glm::vec3 cameraPos = camera.getPosition();

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
    frustumData.maxRenderDistance = GraphicsUtils::calculateRenderDistance(maxLoadDistance);

    {
        ZoneScopedN("Upload Frustum Data");
        _bufferManager.uploadToBuffer(_bufferMgr.getFrustumUniformBuffer(), &frustumData,
                                      sizeof(GPUFrustumData));
    }

    // Host barrier
    {
        VkMemoryBarrier hostBarrier{};
        hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hostBarrier, 0, nullptr,
                             0, nullptr);
    }

    // Reset draw counter
    {
        ZoneScopedN("Reset Draw Counter");
        vkCmdFillBuffer(cmd, _bufferMgr.getCulledIndirectBuffer().buffer, 0, sizeof(uint32_t), 0);

        VkMemoryBarrier fillBarrier{};
        fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fillBarrier, 0, nullptr,
                             0, nullptr);
    }

    // Bind compute pipeline
    {
        ZoneScopedN("Bind Compute Pipeline");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          _pipelineManager.getFrustumCullPipeline().getPipeline());

        VkDescriptorSet cullSet = _bufferMgr.getFrustumCullDescriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                _pipelineManager.getFrustumCullPipelineLayout(), 0, 1, &cullSet, 0,
                                nullptr);
    }

    // Push constants
    ComputeCullingPushConstants pushConstants{};
    pushConstants.totalChunks = static_cast<uint32_t>(_bufferMgr.getChunkDrawData().size());
    pushConstants.chunkSize = GraphicsUtils::Chunk::SIZE_FLOAT;
    pushConstants.debugMode = 1; // Skip distance, use frustum only
    pushConstants._padding = 0;

    vkCmdPushConstants(cmd, _pipelineManager.getFrustumCullPipelineLayout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputeCullingPushConstants), &pushConstants);

    // Dispatch
    {
        ZoneScopedN("Compute Dispatch");
        const uint32_t workgroupCount =
            GraphicsUtils::calculateCullWorkgroups(pushConstants.totalChunks);
        vkCmdDispatch(cmd, workgroupCount, 1, 1);
    }

    // Memory barrier: compute write → indirect read
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
}

void VoxelDrawDispatcher::buildCPUIndirectCommands(VkCommandBuffer cmd) {
    ZoneScopedN("Build CPU Indirect Commands");

    const auto& indirectCommands = _bufferMgr.getIndirectCommands();

    _bufferManager.uploadToBuffer(
        AllocatedBuffer{_bufferMgr.getIndirectBuffer(), VK_NULL_HANDLE},
        indirectCommands.data(), indirectCommands.size() * sizeof(VkDrawIndexedIndirectCommand));

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1,
                         &hostBarrier, 0, nullptr, 0, nullptr);
}

void VoxelDrawDispatcher::drawMeshShaderPath(VkCommandBuffer cmd, bool wireframeMode) {
    ZoneScopedN("Mesh Shader Rendering");
    TracyVkZone(_device.getTracyCtx(), cmd, "GPU Mesh Shader Rendering");

    const uint32_t frameIndex = static_cast<uint32_t>(
        _renderer.getFrameNumber() % ChunkBufferManager::CHUNK_BUFFER_COUNT);

    Pipeline& activePipeline = wireframeMode ? _pipelineManager.getMeshShaderWireframePipeline()
                                             : _pipelineManager.getMeshShaderPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline.getPipeline());

    VkDescriptorSet descriptorSets[] = {_bufferMgr.getMeshShaderDescriptorSet(frameIndex),
                                        _bufferMgr.getMeshShaderFragDescriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _pipelineManager.getMeshShaderPipelineLayout(), 0, 2, descriptorSets, 0,
                            nullptr);

    FragmentShaderPushConstants fragPushConstants{};
    fragPushConstants.viewProj = glm::mat4(1.0F);
    fragPushConstants.needsGammaCorrection = _renderer.needsGammaCorrection() ? 1u : 0u;

    vkCmdPushConstants(cmd, _pipelineManager.getMeshShaderPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FragmentShaderPushConstants),
                       &fragPushConstants);

    TaskShaderPushConstants taskPushConstants{};
    taskPushConstants.totalChunks = static_cast<uint32_t>(_bufferMgr.getChunkDrawData().size());
    taskPushConstants.chunkSize = GraphicsUtils::Chunk::SIZE_FLOAT;
    taskPushConstants.maxVerticesPerMeshWorkgroup = GraphicsUtils::Workgroup::MAX_MESH_VERTICES;
    taskPushConstants.maxMeshWorkgroupsPerTask = _pipelineManager.getMaxMeshWorkgroupsPerTask();

    vkCmdPushConstants(cmd, _pipelineManager.getMeshShaderPipelineLayout(),
                       VK_SHADER_STAGE_TASK_BIT_EXT, 68, sizeof(TaskShaderPushConstants),
                       &taskPushConstants);

    {
        ZoneScopedN("Dispatch Mesh Tasks");
        uint32_t taskWorkgroups =
            GraphicsUtils::calculateTaskWorkgroups(taskPushConstants.totalChunks);
        _pipelineManager.getDrawMeshTasksFunc()(cmd, taskWorkgroups, 1, 1);
    }
}

void VoxelDrawDispatcher::drawTraditionalPath(VkCommandBuffer cmd, Camera& camera,
                                              bool wireframeMode, bool enableGPUCulling) {
    ZoneScopedN("Traditional Rendering");
    TracyVkZone(_device.getTracyCtx(), cmd, "GPU Traditional Rendering");

    const uint32_t frameIndex = static_cast<uint32_t>(
        _renderer.getFrameNumber() % ChunkBufferManager::CHUNK_BUFFER_COUNT);

    Pipeline& activePipeline = wireframeMode ? _pipelineManager.getVoxelWireframePipeline()
                                             : _pipelineManager.getVoxelPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline.getPipeline());

    VkDescriptorSet activeChunkSet = enableGPUCulling ? _bufferMgr.getCulledChunkDescriptorSet()
                                                      : _bufferMgr.getChunkDescriptorSet(frameIndex);

    VkDescriptorSet descriptorSets[] = {activeChunkSet, _bufferMgr.getTraditionalFragDescriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _pipelineManager.getVoxelPipelineLayout(), 0, 2, descriptorSets, 0,
                            nullptr);

    VkExtent2D drawExtent = _context.getDrawExtent();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 projection =
        GraphicsUtils::createVulkanProjectionFromExtent(drawExtent.width, drawExtent.height);
    glm::mat4 viewProjection = projection * view;

    VoxelVertexPushConstants pushConstants;
    pushConstants.viewProj = viewProjection;
    pushConstants.needsGammaCorrection = _renderer.needsGammaCorrection() ? 1u : 0u;

    vkCmdPushConstants(cmd, _pipelineManager.getVoxelPipelineLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(VoxelVertexPushConstants), &pushConstants);

    // Bind mega index buffer
    VkBuffer indexBuffer = _bufferMgr.getIndexBuffer();
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    const auto& chunkDrawData = _bufferMgr.getChunkDrawData();
    const auto& indirectCommands = _bufferMgr.getIndirectCommands();

    if (enableGPUCulling && _pipelineManager.supportsDrawIndirectCount()) {
        vkCmdDrawIndexedIndirectCount(cmd, _bufferMgr.getCulledIndirectBuffer().buffer,
                                      sizeof(uint32_t), _bufferMgr.getCulledIndirectBuffer().buffer,
                                      0, static_cast<uint32_t>(chunkDrawData.size()),
                                      sizeof(VkDrawIndexedIndirectCommand));
    } else if (enableGPUCulling) {
        vkCmdDrawIndexedIndirect(cmd, _bufferMgr.getCulledIndirectBuffer().buffer, sizeof(uint32_t),
                                 static_cast<uint32_t>(chunkDrawData.size()),
                                 sizeof(VkDrawIndexedIndirectCommand));
    } else {
        vkCmdDrawIndexedIndirect(cmd, _bufferMgr.getIndirectBuffer(), 0,
                                 static_cast<uint32_t>(indirectCommands.size()),
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
}


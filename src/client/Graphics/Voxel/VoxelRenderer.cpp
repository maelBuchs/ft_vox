#include "VoxelRenderer.hpp"

#include <iostream>

#include <tracy/Tracy.hpp>

#include "../Core/VulkanBuffer.hpp"
#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
#include "../Renderer.hpp"
#include "../Rendering/CommandExecutor.hpp"
#include "../Rendering/RenderContext.hpp"
#include "ChunkBufferManager.hpp"
#include "VoxelDrawDispatcher.hpp"
#include "VoxelPipelineManager.hpp"
#include "common/Util/ThreadSafeQueue.hpp"

VoxelRenderer::VoxelRenderer(VulkanDevice& device, MeshManager& /*meshManager*/,
                             BlockRegistry& /*registry*/, RenderContext& context,
                             CommandExecutor& executor, VulkanBuffer& bufferManager,
                             DescriptorAllocatorGrowable& descriptorAllocator, Renderer& renderer,
                             std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>>& perThreadMeshQueues)
    : _device(device), _context(context), _renderer(renderer) {
    // Create pipeline manager
    _pipelineManager = std::make_unique<VoxelPipelineManager>(device, context, descriptorAllocator);

    // Create buffer manager (needs pipeline manager for layouts)
    _bufferManager = std::make_unique<ChunkBufferManager>(
        device, bufferManager, descriptorAllocator, executor, renderer, *_pipelineManager,
        perThreadMeshQueues);

    // Create draw dispatcher (deferred to initPipelines since it needs initialized managers)
    _drawDispatcher = std::make_unique<VoxelDrawDispatcher>(
        device, bufferManager, context, renderer, *_pipelineManager, *_bufferManager);
}

VoxelRenderer::~VoxelRenderer() {
    _drawDispatcher.reset();
    _bufferManager.reset();
    _pipelineManager.reset();
}

void VoxelRenderer::initPipelines(VkImageView atlasView, VkSampler atlasSampler,
                                  int texturesPerRow) {
    ZoneScoped;

    // Initialize pipeline manager
    _pipelineManager->initDescriptorLayouts();
    _pipelineManager->initComputeCullingPipeline();

    // Initialize mesh shader pipelines (may fail if not supported)
    bool meshShadersSupported = _pipelineManager->initMeshShaderPipelines();

    // Only initialize traditional pipelines if mesh shaders are not supported
    if (!meshShadersSupported) {
        std::cout << "[VoxelRenderer] Mesh shaders unavailable, using traditional voxel pipeline\n";
        _pipelineManager->initTraditionalPipelines();
    } else {
        std::cout << "[VoxelRenderer] Mesh shader pipeline enabled\n";
    }

    // Initialize buffer manager with atlas
    _bufferManager->init(atlasView, atlasSampler, texturesPerRow);
}

void VoxelRenderer::update(const glm::ivec3& cameraChunkPos, int maxLoadDistance) {
    ZoneScoped;
    _bufferManager->update(cameraChunkPos, maxLoadDistance);
}

void VoxelRenderer::drawVoxels(VkCommandBuffer cmd, Camera& camera, bool wireframeMode) {
    ZoneScoped;

    // Begin frame processing
    _bufferManager->beginFrame();

    // Dispatch draw commands
    if (_drawDispatcher) {
        _drawDispatcher->draw(cmd, camera, wireframeMode, _enableGPUCulling);
    }
}

void VoxelRenderer::rebuildMeshPool(const std::vector<glm::ivec3>& unloadedChunks) {
    _bufferManager->rebuildMeshPool(unloadedChunks);
}

size_t VoxelRenderer::getLoadedChunkCount() const {
    return _bufferManager->getLoadedChunkCount();
}

float VoxelRenderer::getMeshPoolUsage() const {
    return _bufferManager->getMeshPoolUsage();
}

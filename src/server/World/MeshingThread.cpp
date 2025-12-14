#include "MeshingThread.hpp"

#include <iostream>

#include <tracy/Tracy.hpp>

#include "common/World/ChunkMesh.hpp"


MeshingThread::MeshingThread(ThreadSafeQueue<GenerationTask>& taskQueue,
                             ThreadSafeQueue<MeshData>& meshQueue,
                             ThreadSafeQueue<MeshingComplete>& completionQueue,
                             const BlockRegistry& blockRegistry,
                             const TextureIdResolver& textureResolver)
    : _taskQueue(taskQueue), _meshQueue(meshQueue), _completionQueue(completionQueue),
      _blockRegistry(blockRegistry), _textureResolver(textureResolver) {

    // DYNAMIC: Size based on actual block count from JSON
    static const std::string topStr = "top";
    static const std::string bottomStr = "bottom";
    static const std::string sideStr = "side";

    size_t blockCount = blockRegistry.getBlockCount();
    _textureCache.resize(blockCount * 3); // 3 face types per block

    for (size_t blockId = 0; blockId < blockCount; ++blockId) {
        const std::string& topPath = blockRegistry.getTexturePath(blockId, topStr);
        const std::string& bottomPath = blockRegistry.getTexturePath(blockId, bottomStr);
        const std::string& sidePath = blockRegistry.getTexturePath(blockId, sideStr);

        _textureCache[blockId * 3 + 0] = textureResolver(topPath);
        _textureCache[blockId * 3 + 1] = textureResolver(bottomPath);
        _textureCache[blockId * 3 + 2] = textureResolver(sidePath);
    }

    std::cout << "[MeshingThread] Built texture cache for " << blockCount << " block types\n";
}

MeshingThread::~MeshingThread() {
    stop();
}

void MeshingThread::start() {
    if (_running.load()) {
        std::cerr << "[MeshingThread] Already running!\n";
        return;
    }

    _running.store(true);
    _meshingThread = std::make_unique<std::thread>(&MeshingThread::meshingThreadLoop, this);
}

void MeshingThread::stop() {
    if (!_running.load()) {
        return;
    }

    _running.store(false);
    _taskQueue.shutdown(); // Wake up the thread if it's waiting

    if (_meshingThread && _meshingThread->joinable()) {
        _meshingThread->join();
    }
}

void MeshingThread::meshingThreadLoop() {
    ZoneScoped;
    GenerationTask task;

    while (_running.load()) {
        // Block until a meshing task arrives
        if (!_taskQueue.wait_and_pop(task)) {
            // Queue shutdown signal received
            break;
        }

        if (!task.chunkData) {
            std::cerr << "[MeshingThread] Received null chunk data!\n";
            continue;
        }

        // Generate mesh using ChunkMesh::generateMesh with pre-built texture cache
        std::vector<VoxelVertex> vertices;
        std::vector<uint32_t> indices;

        {
            ZoneScopedN("ChunkMesh::generateMesh");

            // This copies border blocks from neighbors for efficient AO calculation
            task.chunkData->buildPadding(
                task.neighborNorth.get(), task.neighborSouth.get(),
                task.neighborEast.get(), task.neighborWest.get(),
                task.neighborTop.get(), task.neighborBottom.get());

            ChunkMesh::generateMesh(*task.chunkData, _blockRegistry, vertices, indices,
                                    task.neighborNorth.get(), task.neighborSouth.get(),
                                    task.neighborEast.get(), task.neighborWest.get(),
                                    task.neighborTop.get(), task.neighborBottom.get(), _textureCache);
        }

        // Always deliver the mesh result to the renderer, even when it's empty
        {
            ZoneScopedN("Push to Mesh Queue");
            MeshData meshData(task.chunkPosition, std::move(vertices), std::move(indices));
            _meshQueue.push(std::move(meshData));
        }

        // Always notify completion (even if mesh is empty)
        _completionQueue.push(MeshingComplete(task.chunkPosition));
    }
}

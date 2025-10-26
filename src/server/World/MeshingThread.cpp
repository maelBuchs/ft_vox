#include "MeshingThread.hpp"

#include <iostream>

#include "common/World/ChunkMesh.hpp"

MeshingThread::MeshingThread(ThreadSafeQueue<GenerationTask>& taskQueue,
                             ThreadSafeQueue<MeshData>& meshQueue,
                             ThreadSafeQueue<MeshingComplete>& completionQueue,
                             const BlockRegistry& blockRegistry,
                             const TextureIdResolver& textureResolver)
    : _taskQueue(taskQueue), _meshQueue(meshQueue), _completionQueue(completionQueue),
      _blockRegistry(blockRegistry), _textureResolver(textureResolver) {}

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
    std::cout << "[MeshingThread] Meshing thread started\n";
}

void MeshingThread::stop() {
    if (!_running.load()) {
        return;
    }

    std::cout << "[MeshingThread] Stopping meshing thread...\n";
    _running.store(false);
    _taskQueue.shutdown(); // Wake up the thread if it's waiting

    if (_meshingThread && _meshingThread->joinable()) {
        _meshingThread->join();
    }

    std::cout << "[MeshingThread] Meshing thread stopped\n";
}

void MeshingThread::meshingThreadLoop() {
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

        // Generate mesh using ChunkMesh::generateMesh
        std::vector<VoxelVertex> vertices;
        std::vector<uint32_t> indices;

        ChunkMesh::generateMesh(
            *task.chunkData, _blockRegistry, vertices, indices, task.neighborNorth.get(),
            task.neighborSouth.get(), task.neighborEast.get(), task.neighborWest.get(),
            task.neighborTop.get(), task.neighborBottom.get(), _textureResolver);

        // Always deliver the mesh result to the renderer, even when it's empty
        MeshData meshData(task.chunkPosition, std::move(vertices), std::move(indices));
        _meshQueue.push(std::move(meshData));

        // Always notify completion (even if mesh is empty)
        _completionQueue.push(MeshingComplete(task.chunkPosition));
    }
}

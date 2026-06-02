#include "MeshingThread.hpp"

#include <algorithm>
#include <iostream>

#include <glm/glm.hpp>

#include <tracy/Tracy.hpp>

#include "common/World/ChunkMesh.hpp"

namespace {
struct DecodedVoxelVertex {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t normalId;
    uint32_t uvId;
    uint32_t textureId;
    uint32_t ao;
    uint32_t width;
    uint32_t height;
};

DecodedVoxelVertex decodeVoxelVertex(VoxelVertex packed) {
    const uint32_t word0 = static_cast<uint32_t>(packed & 0xFFFFFFFFull);
    const uint32_t word1 = static_cast<uint32_t>(packed >> 32);

    return DecodedVoxelVertex{
        .x = (word0)&0x3Fu,
        .y = (word0 >> 6) & 0x3Fu,
        .z = (word0 >> 12) & 0x3Fu,
        .normalId = (word0 >> 18) & 0x7u,
        .uvId = (word0 >> 21) & 0x3u,
        .textureId = (word0 >> 23) & 0x7Fu,
        .ao = (word0 >> 30) & 0x3u,
        .width = (word1 & 0x1Fu) + 1u,
        .height = ((word1 >> 5) & 0x1Fu) + 1u,
    };
}

VoxelVertex encodeVoxelVertex(const DecodedVoxelVertex& v) {
    uint32_t word0 = 0;
    word0 |= (v.x & 0x3F);
    word0 |= ((v.y & 0x3F) << 6);
    word0 |= ((v.z & 0x3F) << 12);
    word0 |= ((v.normalId & 0x7) << 18);
    word0 |= ((v.uvId & 0x3) << 21);
    word0 |= ((v.textureId & 0x7F) << 23);
    word0 |= ((v.ao & 0x3) << 30);

    uint32_t word1 = 0;
    word1 |= ((v.width - 1u) & 0x1F);
    word1 |= (((v.height - 1u) & 0x1F) << 5);

    return (static_cast<uint64_t>(word1) << 32) | static_cast<uint64_t>(word0);
}

void applyLodQuantization(std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices, uint32_t lodLevel) {
    if (lodLevel == 0 || vertices.empty() || indices.empty()) {
        return;
    }

    const uint32_t step = 1u << std::min(lodLevel, 4u);
    std::vector<glm::ivec3> quantizedPositions;
    quantizedPositions.resize(vertices.size());

    for (size_t i = 0; i < vertices.size(); ++i) {
        DecodedVoxelVertex decoded = decodeVoxelVertex(vertices[i]);
        decoded.x = std::min((decoded.x / step) * step, 63u);
        decoded.y = std::min((decoded.y / step) * step, 63u);
        decoded.z = std::min((decoded.z / step) * step, 63u);
        decoded.width = std::max(1u, decoded.width / step);
        decoded.height = std::max(1u, decoded.height / step);
        vertices[i] = encodeVoxelVertex(decoded);
        quantizedPositions[i] =
            glm::ivec3(static_cast<int>(decoded.x), static_cast<int>(decoded.y), static_cast<int>(decoded.z));
    }

    std::vector<uint32_t> simplifiedIndices;
    simplifiedIndices.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= quantizedPositions.size() || ib >= quantizedPositions.size() || ic >= quantizedPositions.size()) {
            continue;
        }
        if (ia == ib || ib == ic || ia == ic) {
            continue;
        }

        const glm::ivec3 va = quantizedPositions[ia];
        const glm::ivec3 vb = quantizedPositions[ib];
        const glm::ivec3 vc = quantizedPositions[ic];
        const glm::ivec3 ab = vb - va;
        const glm::ivec3 ac = vc - va;
        const glm::ivec3 cross = glm::ivec3(
            ab.y * ac.z - ab.z * ac.y,
            ab.z * ac.x - ab.x * ac.z,
            ab.x * ac.y - ab.y * ac.x);
        if (cross == glm::ivec3(0)) {
            continue;
        }

        simplifiedIndices.push_back(ia);
        simplifiedIndices.push_back(ib);
        simplifiedIndices.push_back(ic);
    }

    if (!simplifiedIndices.empty()) {
        indices.swap(simplifiedIndices);
    }
}
} // namespace


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

            auto* chunkPtr = task.chunkData.get();
            if (!chunkPtr->isPaddingValid(task.neighborNorth.get(), task.neighborSouth.get(),
                                           task.neighborEast.get(), task.neighborWest.get(),
                                           task.neighborTop.get(), task.neighborBottom.get())) {
                task.chunkData->buildPadding(
                    task.neighborNorth.get(), task.neighborSouth.get(),
                    task.neighborEast.get(), task.neighborWest.get(),
                    task.neighborTop.get(), task.neighborBottom.get());
            }

            ChunkMesh::generateMesh(*task.chunkData, _blockRegistry, vertices, indices,
                                    task.neighborNorth.get(), task.neighborSouth.get(),
                                    task.neighborEast.get(), task.neighborWest.get(),
                                    task.neighborTop.get(), task.neighborBottom.get(), _textureCache);
            applyLodQuantization(vertices, indices, task.lodLevel);
        }

        // Always deliver the mesh result to the renderer, even when it's empty
        {
            ZoneScopedN("Push to Mesh Queue");
            MeshData meshData(task.chunkPosition, std::move(vertices), std::move(indices), task.lodLevel);
            _meshQueue.push(std::move(meshData));
        }

        // Always notify completion (even if mesh is empty)
        _completionQueue.push(MeshingComplete(task.chunkPosition, task.lodLevel));
    }
}

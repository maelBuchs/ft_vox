#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>

#include "glm/fwd.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#define RENDER_DISTANCE_IN_CHUNKS 4

class Chunk;
using chunkMap = std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>>;

/*
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
    }*/

class Chunk {
  public:
    static constexpr int CHUNK_SIZE = 32;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    static constexpr uint8_t AIR_BLOCK_ID = 0;

    Chunk(int x, int y, int z);
    Chunk() = default;
    ~Chunk() = default;

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = default;
    Chunk& operator=(Chunk&&) = default;

    // Block access (using block IDs from BlockRegistry)
    [[nodiscard]] uint8_t getBlock(int x, int y, int z) const;
    void setBlock(int x, int y, int z, uint8_t blockId);

    // Helper functions
    [[nodiscard]] bool isBlockSolid(int x, int y, int z) const;
    [[nodiscard]] bool isInBounds(int x, int y, int z) const;
    [[nodiscard]] int getIndex(int x, int y, int z) const;

    // Chunk state
    [[nodiscard]] bool isEmpty() const { return _isEmpty; }
    void setEmpty(bool empty) { _isEmpty = empty; }
    std::tuple<int, int, int> getPosition() const { return position; }

  private:
    std::tuple<int, int, int> position;
    std::array<uint8_t, VOLUME> _blocks;
    bool _isEmpty = true;
};

class ChunkInstanciator {
  public:
    ChunkInstanciator() = default;
    ~ChunkInstanciator() = default;
    ChunkInstanciator(const ChunkInstanciator&) = delete;
    ChunkInstanciator& operator=(const ChunkInstanciator&) = delete;
    ChunkInstanciator(ChunkInstanciator&&) = default;
    ChunkInstanciator& operator=(ChunkInstanciator&&) = default;
    // hecks which chunks need to be loaded/unloaded based on player position

    void updateChunksAroundPlayer(float playerX, float playerY, float playerZ, float viewDistance);

  private:
    void loadChunkAt(int x, int y, int z);
    void unloadChunkAt(int x, int y, int z);
    // some data structures to hold loaded chunks
    chunkMap _loadedChunks;
};

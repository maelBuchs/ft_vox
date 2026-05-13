#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "common/Types/RenderTypes.hpp"

// Forward declaration
class Chunk;

// Client -> Server: Request to load/generate a chunk
struct ChunkRequest {
    glm::ivec3 chunkPosition;
    uint32_t lodLevel = 0;

    ChunkRequest() = default;
    explicit ChunkRequest(const glm::ivec3& pos, uint32_t lod = 0) : chunkPosition(pos), lodLevel(lod) {}
};

// Server -> Meshing Thread: Chunk data ready for meshing
struct GenerationTask {
    glm::ivec3 chunkPosition;
    uint32_t lodLevel = 0;
    std::shared_ptr<Chunk> chunkData; // The main chunk to mesh

    // Neighbor chunks for face culling (can be nullptr if not loaded)
    std::shared_ptr<Chunk> neighborNorth;  // +Z
    std::shared_ptr<Chunk> neighborSouth;  // -Z
    std::shared_ptr<Chunk> neighborEast;   // +X
    std::shared_ptr<Chunk> neighborWest;   // -X
    std::shared_ptr<Chunk> neighborTop;    // +Y
    std::shared_ptr<Chunk> neighborBottom; // -Y

    GenerationTask() = default;
    explicit GenerationTask(const glm::ivec3& pos, std::shared_ptr<Chunk> chunk, uint32_t lod = 0)
        : chunkPosition(pos), lodLevel(lod), chunkData(std::move(chunk)) {}
};

// Meshing Thread -> Client: Finished mesh data ready for GPU upload
struct MeshData {
    glm::ivec3 chunkPosition;
    uint32_t lodLevel = 0;
    std::vector<VoxelVertex> vertices; // Packed vertex data
    std::vector<uint32_t> indices;

    MeshData() = default;
    MeshData(const glm::ivec3& pos, std::vector<VoxelVertex> verts, std::vector<uint32_t> idx,
             uint32_t lod = 0)
        : chunkPosition(pos), lodLevel(lod), vertices(std::move(verts)), indices(std::move(idx)) {}
};

// Meshing completion notification (chunk position only)
struct MeshingComplete {
    glm::ivec3 chunkPosition;
    uint32_t lodLevel = 0;

    MeshingComplete() = default;
    explicit MeshingComplete(const glm::ivec3& pos, uint32_t lod = 0)
        : chunkPosition(pos), lodLevel(lod) {}
};

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "BlockRegistry.hpp"
#include "Chunk.hpp"
#include "common/Types/RenderTypes.hpp"
class Chunk;

class ChunkMesh {
  public:
    ChunkMesh() = default;
    ~ChunkMesh() = default;

    ChunkMesh(const ChunkMesh&) = delete;
    ChunkMesh& operator=(const ChunkMesh&) = delete;
    ChunkMesh(ChunkMesh&&) = default;
    ChunkMesh& operator=(ChunkMesh&&) = default;

    using TextureIdResolver = std::function<uint32_t(const std::string&)>;

    // Generate mesh from chunk data with neighbor awareness
    // Uses pre-built texture cache for performance (flat array: [blockId * 3 + faceType])
    static void generateMesh(const Chunk& mainChunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                             const Chunk* neighborNorth,  // +Z
                             const Chunk* neighborSouth,  // -Z
                             const Chunk* neighborEast,   // +X
                             const Chunk* neighborWest,   // -X
                             const Chunk* neighborTop,    // +Y
                             const Chunk* neighborBottom, // -Y
                             const std::vector<uint32_t>& textureCache); // Pre-built texture cache

  private:
    enum class FaceDirection { North, South, East, West, Top, Bottom };

    // Add a face to the mesh
    static void addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                        const std::vector<uint32_t>& textureCache, const Chunk& chunk,
                      const BlockRegistry& registry);
};

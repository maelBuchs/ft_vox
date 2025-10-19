#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "BlockRegistry.hpp"
#include "Chunk.hpp"
#include "common/Types/RenderTypes.hpp"

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
    static void generateMesh(const Chunk& mainChunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                             const Chunk* neighborNorth,  // +Z
                             const Chunk* neighborSouth,  // -Z
                             const Chunk* neighborEast,   // +X
                             const Chunk* neighborWest,   // -X
                             const Chunk* neighborTop,    // +Y
                             const Chunk* neighborBottom, // -Y
                             const TextureIdResolver& getTextureId);

  private:
    enum class FaceDirection { North, South, East, West, Top, Bottom };

    // Add a face to the mesh
    static void addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                        const BlockRegistry& registry, const TextureIdResolver& getTextureId);
};

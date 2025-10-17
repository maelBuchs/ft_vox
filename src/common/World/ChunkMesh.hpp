#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "BlockRegistry.hpp"
#include "Chunk.hpp"

// Forward declarations
struct VoxelVertex;
class Chunk;
class BlockRegistry;

class ChunkMesh {
  public:
    ChunkMesh() = default;
    ~ChunkMesh() = default;

    ChunkMesh(const ChunkMesh&) = delete;
    ChunkMesh& operator=(const ChunkMesh&) = delete;
    ChunkMesh(ChunkMesh&&) = default;
    ChunkMesh& operator=(ChunkMesh&&) = default;

    // Generate mesh from chunk data
    static void generateMesh(const Chunk& chunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices);

  private:
    enum class FaceDirection { North, South, East, West, Top, Bottom };

    // Add a face to the mesh
    static void addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        const BlockRegistry& registry, std::vector<VoxelVertex>& vertices,
                        std::vector<uint32_t>& indices);

    // Get color for block ID from registry
    static glm::vec4 getBlockColor(int blockId, const BlockRegistry& registry);
};

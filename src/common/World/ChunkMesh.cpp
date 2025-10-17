#include "ChunkMesh.hpp"

void ChunkMesh::generateMesh(const Chunk& chunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();

    if (chunk.isEmpty()) {
        return;
    }

    for (int x = 0; x < Chunk::CHUNK_SIZE; x++) {
        for (int y = 0; y < Chunk::CHUNK_SIZE; y++) {
            for (int z = 0; z < Chunk::CHUNK_SIZE; z++) {
                int blockId = static_cast<int>(chunk.getBlock(x, y, z));

                // Skip air blocks or non-displayable blocks
                if (blockId == Chunk::AIR_BLOCK_ID || !registry.isDisplayable(blockId)) {
                    continue;
                }

                // Check each face direction and add face if exposed
                // North (positive Z)
                if (!chunk.isBlockSolid(x, y, z + 1)) {
                    addFace(FaceDirection::North, x, y, z, blockId, registry, vertices, indices);
                }

                // South (negative Z)
                if (!chunk.isBlockSolid(x, y, z - 1)) {
                    addFace(FaceDirection::South, x, y, z, blockId, registry, vertices, indices);
                }

                // East (positive X)
                if (!chunk.isBlockSolid(x + 1, y, z)) {
                    addFace(FaceDirection::East, x, y, z, blockId, registry, vertices, indices);
                }

                // West (negative X)
                if (!chunk.isBlockSolid(x - 1, y, z)) {
                    addFace(FaceDirection::West, x, y, z, blockId, registry, vertices, indices);
                }

                // Top (positive Y)
                if (!chunk.isBlockSolid(x, y + 1, z)) {
                    addFace(FaceDirection::Top, x, y, z, blockId, registry, vertices, indices);
                }

                // Bottom (negative Y)
                if (!chunk.isBlockSolid(x, y - 1, z)) {
                    addFace(FaceDirection::Bottom, x, y, z, blockId, registry, vertices, indices);
                }
            }
        }
    }

    // Mesh generation complete
}

void ChunkMesh::addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        const BlockRegistry& registry, std::vector<VoxelVertex>& vertices,
                        std::vector<uint32_t>& indices) {
    glm::vec4 color = getBlockColor(blockId, registry);
    auto baseIndex = static_cast<uint32_t>(vertices.size());

    // Create 4 vertices for the face quad
    VoxelVertex v0{};
    VoxelVertex v1{};
    VoxelVertex v2{};
    VoxelVertex v3{};
    glm::vec3 normal{};

    // All vertex definitions are consistently Counter-Clockwise when viewed from outside.
    switch (direction) {
    case FaceDirection::East: // +X
        v0.position = glm::vec3(x + 1, y, z);
        v1.position = glm::vec3(x + 1, y, z + 1);
        v2.position = glm::vec3(x + 1, y + 1, z + 1);
        v3.position = glm::vec3(x + 1, y + 1, z);
        normal = glm::vec3(1.0F, 0.0F, 0.0F);
        break;

    case FaceDirection::West: // -X
        v0.position = glm::vec3(x, y, z + 1);
        v1.position = glm::vec3(x, y, z);
        v2.position = glm::vec3(x, y + 1, z);
        v3.position = glm::vec3(x, y + 1, z + 1);
        normal = glm::vec3(-1.0F, 0.0F, 0.0F);
        break;

    case FaceDirection::Top: // +Y
        v0.position = glm::vec3(x, y + 1, z);
        v1.position = glm::vec3(x + 1, y + 1, z);
        v2.position = glm::vec3(x + 1, y + 1, z + 1);
        v3.position = glm::vec3(x, y + 1, z + 1);
        normal = glm::vec3(0.0F, 1.0F, 0.0F);
        break;

    case FaceDirection::Bottom: // -Y
        v0.position = glm::vec3(x, y, z + 1);
        v1.position = glm::vec3(x + 1, y, z + 1);
        v2.position = glm::vec3(x + 1, y, z);
        v3.position = glm::vec3(x, y, z);
        normal = glm::vec3(0.0F, -1.0F, 0.0F);
        break;

    case FaceDirection::North: // +Z
        v0.position = glm::vec3(x + 1, y, z + 1);
        v1.position = glm::vec3(x, y, z + 1);
        v2.position = glm::vec3(x, y + 1, z + 1);
        v3.position = glm::vec3(x + 1, y + 1, z + 1);
        normal = glm::vec3(0.0F, 0.0F, 1.0F);
        break;

    case FaceDirection::South: // -Z
        v0.position = glm::vec3(x, y, z);
        v1.position = glm::vec3(x + 1, y, z);
        v2.position = glm::vec3(x + 1, y + 1, z);
        v3.position = glm::vec3(x, y + 1, z);
        normal = glm::vec3(0.0F, 0.0F, -1.0F);
        break;
    }

    // Set UVs (standard quad mapping)
    v0.uv_x = 0.0F;
    v0.uv_y = 0.0F;
    v1.uv_x = 1.0F;
    v1.uv_y = 0.0F;
    v2.uv_x = 1.0F;
    v2.uv_y = 1.0F;
    v3.uv_x = 0.0F;
    v3.uv_y = 1.0F;

    // Set normals and colors for all 4 vertices
    v0.normal = v1.normal = v2.normal = v3.normal = normal;
    v0.color = v1.color = v2.color = v3.color = color;

    // Add vertices to the buffer
    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);
    vertices.push_back(v3);

    // Add indices using a consistent CCW triangulation for the quad
    // Triangle 1: v0, v1, v2
    indices.push_back(baseIndex + 0);
    indices.push_back(baseIndex + 1);
    indices.push_back(baseIndex + 2);

    // Triangle 2: v0, v2, v3
    indices.push_back(baseIndex + 0);
    indices.push_back(baseIndex + 2);
    indices.push_back(baseIndex + 3);
}

glm::vec4 ChunkMesh::getBlockColor(int blockId, const BlockRegistry& registry) {
    std::string name = registry.getName(blockId);

    // Map block names to colors - eventually this will use textures
    if (name == "grass_block") {
        return glm::vec4(0.4F, 1.0F, 0.3F, 1.0F); // Bright green
    } else if (name == "dirt") {
        return glm::vec4(0.8F, 0.5F, 0.3F, 1.0F); // Bright brown
    } else if (name == "stone") {
        return glm::vec4(0.7F, 0.7F, 0.7F, 1.0F); // Light gray
    } else if (name == "sand") {
        return glm::vec4(1.0F, 0.95F, 0.6F, 1.0F); // Bright yellow
    } else if (name == "oak_wood") {
        return glm::vec4(0.6F, 0.4F, 0.2F, 1.0F); // Brown wood
    } else if (name == "water") {
        return glm::vec4(0.2F, 0.4F, 1.0F, 0.6F); // Blue water with transparency
    }
    // Default fallback
    return glm::vec4(1.0F, 1.0F, 1.0F, 1.0F); // White
}

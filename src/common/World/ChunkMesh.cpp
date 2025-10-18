#include "ChunkMesh.hpp"

namespace {
// Helper function to pack a vertex's data into a uint32_t
// Bit layout: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][Spare:2]
uint32_t packVertex(uint32_t x, uint32_t y, uint32_t z, uint32_t normalId, uint32_t uvId,
                    uint32_t textureId) {
    uint32_t packedData = 0;

    // 6 bits for X, 6 for Y, 6 for Z
    packedData |= (x & 0x3F);         // X in bits 0-5
    packedData |= ((y & 0x3F) << 6);  // Y in bits 6-11
    packedData |= ((z & 0x3F) << 12); // Z in bits 12-17

    // 3 bits for Normal ID
    packedData |= ((normalId & 0x7) << 18); // Normal ID in bits 18-20

    // 2 bits for UV Corner ID
    packedData |= ((uvId & 0x3) << 21); // UV ID in bits 21-22

    // 7 bits for Texture ID
    packedData |= ((textureId & 0x7F) << 23); // Texture ID in bits 23-29

    // The remaining 2 bits (30-31) are spare

    return packedData;
}
} // anonymous namespace

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
                    addFace(FaceDirection::North, x, y, z, blockId, vertices, indices);
                }

                // South (negative Z)
                if (!chunk.isBlockSolid(x, y, z - 1)) {
                    addFace(FaceDirection::South, x, y, z, blockId, vertices, indices);
                }

                // East (positive X)
                if (!chunk.isBlockSolid(x + 1, y, z)) {
                    addFace(FaceDirection::East, x, y, z, blockId, vertices, indices);
                }

                // West (negative X)
                if (!chunk.isBlockSolid(x - 1, y, z)) {
                    addFace(FaceDirection::West, x, y, z, blockId, vertices, indices);
                }

                // Top (positive Y)
                if (!chunk.isBlockSolid(x, y + 1, z)) {
                    addFace(FaceDirection::Top, x, y, z, blockId, vertices, indices);
                }

                // Bottom (negative Y)
                if (!chunk.isBlockSolid(x, y - 1, z)) {
                    addFace(FaceDirection::Bottom, x, y, z, blockId, vertices, indices);
                }
            }
        }
    }

    // Mesh generation complete
}

void ChunkMesh::addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices) {
    // For now, use blockId as textureId. Later this will be a lookup.
    uint32_t textureId = static_cast<uint32_t>(blockId);

    // Get the base index for the new vertices
    auto baseIndex = static_cast<uint32_t>(vertices.size());

    uint32_t px = static_cast<uint32_t>(x);
    uint32_t py = static_cast<uint32_t>(y);
    uint32_t pz = static_cast<uint32_t>(z);

    uint32_t normalId = 0;
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    uint32_t v3 = 0;

    // Define the 4 vertices of the quad based on face direction
    // All vertex definitions are consistently Counter-Clockwise when viewed from outside.
    switch (direction) {
    case FaceDirection::East: // +X
        normalId = 0;
        v0 = packVertex(px + 1, py, pz, normalId, 0, textureId);         // Bottom-left
        v1 = packVertex(px + 1, py, pz + 1, normalId, 1, textureId);     // Bottom-right
        v2 = packVertex(px + 1, py + 1, pz + 1, normalId, 2, textureId); // Top-right
        v3 = packVertex(px + 1, py + 1, pz, normalId, 3, textureId);     // Top-left
        break;
    case FaceDirection::West: // -X
        normalId = 1;
        v0 = packVertex(px, py, pz + 1, normalId, 0, textureId);
        v1 = packVertex(px, py, pz, normalId, 1, textureId);
        v2 = packVertex(px, py + 1, pz, normalId, 2, textureId);
        v3 = packVertex(px, py + 1, pz + 1, normalId, 3, textureId);
        break;
    case FaceDirection::Top: // +Y
        normalId = 2;
        v0 = packVertex(px, py + 1, pz, normalId, 0, textureId);
        v1 = packVertex(px + 1, py + 1, pz, normalId, 1, textureId);
        v2 = packVertex(px + 1, py + 1, pz + 1, normalId, 2, textureId);
        v3 = packVertex(px, py + 1, pz + 1, normalId, 3, textureId);
        break;
    case FaceDirection::Bottom: // -Y
        normalId = 3;
        v0 = packVertex(px, py, pz + 1, normalId, 0, textureId);
        v1 = packVertex(px + 1, py, pz + 1, normalId, 1, textureId);
        v2 = packVertex(px + 1, py, pz, normalId, 2, textureId);
        v3 = packVertex(px, py, pz, normalId, 3, textureId);
        break;
    case FaceDirection::North: // +Z
        normalId = 4;
        v0 = packVertex(px + 1, py, pz + 1, normalId, 0, textureId);
        v1 = packVertex(px, py, pz + 1, normalId, 1, textureId);
        v2 = packVertex(px, py + 1, pz + 1, normalId, 2, textureId);
        v3 = packVertex(px + 1, py + 1, pz + 1, normalId, 3, textureId);
        break;
    case FaceDirection::South: // -Z
        normalId = 5;
        v0 = packVertex(px, py, pz, normalId, 0, textureId);
        v1 = packVertex(px + 1, py, pz, normalId, 1, textureId);
        v2 = packVertex(px + 1, py + 1, pz, normalId, 2, textureId);
        v3 = packVertex(px, py + 1, pz, normalId, 3, textureId);
        break;
    }

    // Should never reach here, all enum values are covered
    if (normalId == 0 && v0 == 0) {
        return; // Defensive programming
    }

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

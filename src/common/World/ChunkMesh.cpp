#include "ChunkMesh.hpp"

namespace {
// Helper function to pack a vertex's data into a uint32_t
// Bit layout: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][AO:2]
uint32_t packVertex(uint32_t x, uint32_t y, uint32_t z, uint32_t normalId, uint32_t uvId,
                    uint32_t textureId, uint32_t ao) {
    uint32_t packedData = 0;

    packedData |= (x & 0x3F);
    packedData |= ((y & 0x3F) << 6);
    packedData |= ((z & 0x3F) << 12);
    packedData |= ((normalId & 0x7) << 18);
    packedData |= ((uvId & 0x3) << 21);
    packedData |= ((textureId & 0x7F) << 23);
    packedData |= ((ao & 0x3) << 30);

    return packedData;
}

// AO Calculation function
uint32_t calculateAO(bool side1, bool side2, bool corner) {
    if (side1 && side2) {
        return 3; // Fully occluded
    }
    return static_cast<uint32_t>(side1) + static_cast<uint32_t>(side2) +
           static_cast<uint32_t>(corner);
}
} // anonymous namespace

void ChunkMesh::generateMesh(const Chunk& mainChunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                             const Chunk* neighborNorth, const Chunk* neighborSouth,
                             const Chunk* neighborEast, const Chunk* neighborWest,
                             const Chunk* neighborTop, const Chunk* neighborBottom,
                             const TextureIdResolver& getTextureId) {
    vertices.clear();
    indices.clear();

    if (mainChunk.isEmpty()) {
        return;
    }

    for (int x = 0; x < Chunk::CHUNK_SIZE; x++) {
        for (int y = 0; y < Chunk::CHUNK_SIZE; y++) {
            for (int z = 0; z < Chunk::CHUNK_SIZE; z++) {
                int blockId = static_cast<int>(mainChunk.getBlock(x, y, z));

                // Skip air blocks or non-displayable blocks
                if (blockId == Chunk::AIR_BLOCK_ID || !registry.isDisplayable(blockId)) {
                    continue;
                }

                // North (+Z)
                bool isNorthSolid =
                    (z == Chunk::CHUNK_SIZE - 1)
                        ? (neighborNorth != nullptr && neighborNorth->isBlockSolid(x, y, 0))
                        : mainChunk.isBlockSolid(x, y, z + 1);
                if (!isNorthSolid) {
                    addFace(FaceDirection::North, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }

                // South (-Z)
                bool isSouthSolid = (z == 0)
                                        ? (neighborSouth != nullptr &&
                                           neighborSouth->isBlockSolid(x, y, Chunk::CHUNK_SIZE - 1))
                                        : mainChunk.isBlockSolid(x, y, z - 1);
                if (!isSouthSolid) {
                    addFace(FaceDirection::South, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }

                // East (+X)
                bool isEastSolid =
                    (x == Chunk::CHUNK_SIZE - 1)
                        ? (neighborEast != nullptr && neighborEast->isBlockSolid(0, y, z))
                        : mainChunk.isBlockSolid(x + 1, y, z);
                if (!isEastSolid) {
                    addFace(FaceDirection::East, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }

                // West (-X)
                bool isWestSolid = (x == 0)
                                       ? (neighborWest != nullptr &&
                                          neighborWest->isBlockSolid(Chunk::CHUNK_SIZE - 1, y, z))
                                       : mainChunk.isBlockSolid(x - 1, y, z);
                if (!isWestSolid) {
                    addFace(FaceDirection::West, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }

                // Top (+Y)
                bool isTopSolid =
                    (y == Chunk::CHUNK_SIZE - 1)
                        ? (neighborTop != nullptr && neighborTop->isBlockSolid(x, 0, z))
                        : mainChunk.isBlockSolid(x, y + 1, z);
                if (!isTopSolid) {
                    addFace(FaceDirection::Top, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }

                // Bottom (-Y)
                bool isBottomSolid =
                    (y == 0) ? (neighborBottom != nullptr &&
                                neighborBottom->isBlockSolid(x, Chunk::CHUNK_SIZE - 1, z))
                             : mainChunk.isBlockSolid(x, y - 1, z);
                if (!isBottomSolid) {
                    addFace(FaceDirection::Bottom, x, y, z, blockId, vertices, indices, registry,
                            getTextureId, mainChunk);
                }
            }
        }
    }

    // Mesh generation complete
}

void ChunkMesh::addFace(FaceDirection direction, int x, int y, int z, int blockId,
                        std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                        const BlockRegistry& registry, const TextureIdResolver& getTextureId,
                        const Chunk& chunk) {
    std::string faceName;
    switch (direction) {
    case FaceDirection::Top:
        faceName = "top";
        break;
    case FaceDirection::Bottom:
        faceName = "bottom";
        break;
    default:
        faceName = "side";
        break;
    }

    std::string texturePath = registry.getTexturePath(blockId, faceName);
    uint32_t textureId = getTextureId(texturePath);

    auto baseIndex = static_cast<uint32_t>(vertices.size());
    uint32_t px = static_cast<uint32_t>(x);
    uint32_t py = static_cast<uint32_t>(y);
    uint32_t pz = static_cast<uint32_t>(z);
    uint32_t normalId = 0;

    bool s1, s2, c; // side1, side2, corner for AO
    uint32_t ao[4]; // 0: BL, 1: BR, 2: TR, 3: TL

    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    uint32_t v3 = 0;

    switch (direction) {
    case FaceDirection::East: // +X
        normalId = 0;
        s1 = chunk.isBlockSolid(x + 1, y + 1, z);
        c = chunk.isBlockSolid(x + 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x + 1, y, z - 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y + 1, z);
        c = chunk.isBlockSolid(x + 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x + 1, y, z + 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y - 1, z);
        c = chunk.isBlockSolid(x + 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x + 1, y, z + 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y - 1, z);
        c = chunk.isBlockSolid(x + 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x + 1, y, z - 1);
        ao[0] = calculateAO(s1, s2, c);
        v0 = packVertex(px + 1, py, pz, normalId, 0, textureId, ao[0]);
        v1 = packVertex(px + 1, py, pz + 1, normalId, 1, textureId, ao[1]);
        v2 = packVertex(px + 1, py + 1, pz + 1, normalId, 2, textureId, ao[2]);
        v3 = packVertex(px + 1, py + 1, pz, normalId, 3, textureId, ao[3]);
        break;
    case FaceDirection::West: // -X
        normalId = 1;
        s1 = chunk.isBlockSolid(x - 1, y + 1, z);
        c = chunk.isBlockSolid(x - 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x - 1, y, z + 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y + 1, z);
        c = chunk.isBlockSolid(x - 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x - 1, y, z - 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y - 1, z);
        c = chunk.isBlockSolid(x - 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x - 1, y, z - 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y - 1, z);
        c = chunk.isBlockSolid(x - 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x - 1, y, z + 1);
        ao[0] = calculateAO(s1, s2, c);
        v0 = packVertex(px, py, pz + 1, normalId, 1, textureId, ao[0]);
        v1 = packVertex(px, py, pz, normalId, 0, textureId, ao[1]);
        v2 = packVertex(px, py + 1, pz, normalId, 3, textureId, ao[2]);
        v3 = packVertex(px, py + 1, pz + 1, normalId, 2, textureId, ao[3]);
        break;
    case FaceDirection::Top: // +Y
        normalId = 2;
        s1 = chunk.isBlockSolid(x, y + 1, z - 1);
        c = chunk.isBlockSolid(x - 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x - 1, y + 1, z);
        ao[0] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y + 1, z - 1);
        c = chunk.isBlockSolid(x + 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x + 1, y + 1, z);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y + 1, z + 1);
        c = chunk.isBlockSolid(x + 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x + 1, y + 1, z);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y + 1, z + 1);
        c = chunk.isBlockSolid(x - 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x - 1, y + 1, z);
        ao[3] = calculateAO(s1, s2, c);
        v0 = packVertex(px, py + 1, pz, normalId, 0, textureId, ao[0]);
        v1 = packVertex(px + 1, py + 1, pz, normalId, 1, textureId, ao[1]);
        v2 = packVertex(px + 1, py + 1, pz + 1, normalId, 2, textureId, ao[2]);
        v3 = packVertex(px, py + 1, pz + 1, normalId, 3, textureId, ao[3]);
        break;
    case FaceDirection::Bottom: // -Y
        normalId = 3;
        s1 = chunk.isBlockSolid(x, y - 1, z + 1);
        c = chunk.isBlockSolid(x - 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x - 1, y - 1, z);
        ao[0] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y - 1, z + 1);
        c = chunk.isBlockSolid(x + 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x + 1, y - 1, z);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y - 1, z - 1);
        c = chunk.isBlockSolid(x + 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x + 1, y - 1, z);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x, y - 1, z - 1);
        c = chunk.isBlockSolid(x - 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x - 1, y - 1, z);
        ao[3] = calculateAO(s1, s2, c);
        v0 = packVertex(px, py, pz + 1, normalId, 3, textureId, ao[0]);
        v1 = packVertex(px + 1, py, pz + 1, normalId, 2, textureId, ao[1]);
        v2 = packVertex(px + 1, py, pz, normalId, 1, textureId, ao[2]);
        v3 = packVertex(px, py, pz, normalId, 0, textureId, ao[3]);
        break;
    case FaceDirection::North: // +Z
        normalId = 4;
        s1 = chunk.isBlockSolid(x + 1, y, z + 1);
        c = chunk.isBlockSolid(x + 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x, y + 1, z + 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y, z + 1);
        c = chunk.isBlockSolid(x - 1, y + 1, z + 1);
        s2 = chunk.isBlockSolid(x, y + 1, z + 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y, z + 1);
        c = chunk.isBlockSolid(x - 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x, y - 1, z + 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y, z + 1);
        c = chunk.isBlockSolid(x + 1, y - 1, z + 1);
        s2 = chunk.isBlockSolid(x, y - 1, z + 1);
        ao[0] = calculateAO(s1, s2, c);
        v0 = packVertex(px + 1, py, pz + 1, normalId, 1, textureId, ao[0]);
        v1 = packVertex(px, py, pz + 1, normalId, 0, textureId, ao[1]);
        v2 = packVertex(px, py + 1, pz + 1, normalId, 3, textureId, ao[2]);
        v3 = packVertex(px + 1, py + 1, pz + 1, normalId, 2, textureId, ao[3]);
        break;
    case FaceDirection::South: // -Z
        normalId = 5;
        s1 = chunk.isBlockSolid(x - 1, y, z - 1);
        c = chunk.isBlockSolid(x - 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x, y + 1, z - 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y, z - 1);
        c = chunk.isBlockSolid(x + 1, y + 1, z - 1);
        s2 = chunk.isBlockSolid(x, y + 1, z - 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x + 1, y, z - 1);
        c = chunk.isBlockSolid(x + 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x, y - 1, z - 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = chunk.isBlockSolid(x - 1, y, z - 1);
        c = chunk.isBlockSolid(x - 1, y - 1, z - 1);
        s2 = chunk.isBlockSolid(x, y - 1, z - 1);
        ao[0] = calculateAO(s1, s2, c);
        v0 = packVertex(px, py, pz, normalId, 0, textureId, ao[0]);
        v1 = packVertex(px + 1, py, pz, normalId, 1, textureId, ao[1]);
        v2 = packVertex(px + 1, py + 1, pz, normalId, 2, textureId, ao[2]);
        v3 = packVertex(px, py + 1, pz, normalId, 3, textureId, ao[3]);
        break;
    }

    // Flip quad diagonal to fix AO artifacts
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        vertices.push_back(v0);
        vertices.push_back(v1);
        vertices.push_back(v2);
        vertices.push_back(v3);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 2);
    } else {
        vertices.push_back(v0);
        vertices.push_back(v1);
        vertices.push_back(v2);
        vertices.push_back(v3);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 2);
    }
}

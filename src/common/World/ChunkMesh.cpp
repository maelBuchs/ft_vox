#include "ChunkMesh.hpp"

#include <array>
#include <cstring>

#include <tracy/Tracy.hpp>

namespace {

// Word 0: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][AO:2] = 32 bits
// Word 1: [Width:5][Height:5][Reserved:22] = 32 bits
uint64_t packGreedyVertex(uint32_t x, uint32_t y, uint32_t z, uint32_t normalId, uint32_t uvId,
                          uint32_t textureId, uint32_t ao, uint32_t width, uint32_t height) {
    uint32_t word0 = 0;
    word0 |= (x & 0x3F);
    word0 |= ((y & 0x3F) << 6);
    word0 |= ((z & 0x3F) << 12);
    word0 |= ((normalId & 0x7) << 18);
    word0 |= ((uvId & 0x3) << 21);
    word0 |= ((textureId & 0x7F) << 23);
    word0 |= ((ao & 0x3) << 30);

    uint32_t word1 = 0;
    word1 |= ((width - 1) & 0x1F);
    word1 |= (((height - 1) & 0x1F) << 5);

    return (static_cast<uint64_t>(word1) << 32) | static_cast<uint64_t>(word0);
}

uint32_t calculateAO(bool side1, bool side2, bool corner) {
    if (side1 && side2)
        return 3;
    return static_cast<uint32_t>(side1) + static_cast<uint32_t>(side2) +
           static_cast<uint32_t>(corner);
}

struct FaceData {
    int16_t blockId;
    uint8_t textureId;
    uint8_t ao[4];

    bool operator==(const FaceData& other) const {
        return blockId == other.blockId && textureId == other.textureId && ao[0] == other.ao[0] &&
               ao[1] == other.ao[1] && ao[2] == other.ao[2] && ao[3] == other.ao[3];
    }

    bool isValid() const { return blockId >= 0; }
};

using SliceMask = std::array<std::array<FaceData, 32>, 32>;

uint32_t getTextureForFace(int blockId, int faceType, const std::vector<uint32_t>& textureCache) {
    return textureCache[(blockId * 3) + faceType];
}

// Simple AO calculation using padded chunk data
// The chunk must have buildPadding() called before meshing
inline void calculateAOForFace(const Chunk& chunk, const BlockRegistry& registry,
                               int x, int y, int z, int normalId, uint8_t ao[4]) {
    bool s1, s2, c;

    // Use padded block access - coordinates can be -1 to CHUNK_SIZE
    auto isSolid = [&](int bx, int by, int bz) {
        return chunk.isPaddedBlockSolid(bx, by, bz, registry);
    };

    switch (normalId) {
    case 0: // +X
        s1 = isSolid(x + 1, y + 1, z);
        c = isSolid(x + 1, y + 1, z - 1);
        s2 = isSolid(x + 1, y, z - 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y + 1, z);
        c = isSolid(x + 1, y + 1, z + 1);
        s2 = isSolid(x + 1, y, z + 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y - 1, z);
        c = isSolid(x + 1, y - 1, z + 1);
        s2 = isSolid(x + 1, y, z + 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y - 1, z);
        c = isSolid(x + 1, y - 1, z - 1);
        s2 = isSolid(x + 1, y, z - 1);
        ao[0] = calculateAO(s1, s2, c);
        break;
    case 1: // -X
        s1 = isSolid(x - 1, y + 1, z);
        c = isSolid(x - 1, y + 1, z + 1);
        s2 = isSolid(x - 1, y, z + 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y + 1, z);
        c = isSolid(x - 1, y + 1, z - 1);
        s2 = isSolid(x - 1, y, z - 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y - 1, z);
        c = isSolid(x - 1, y - 1, z - 1);
        s2 = isSolid(x - 1, y, z - 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y - 1, z);
        c = isSolid(x - 1, y - 1, z + 1);
        s2 = isSolid(x - 1, y, z + 1);
        ao[0] = calculateAO(s1, s2, c);
        break;
    case 2: // +Y
        s1 = isSolid(x, y + 1, z - 1);
        c = isSolid(x - 1, y + 1, z - 1);
        s2 = isSolid(x - 1, y + 1, z);
        ao[0] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y + 1, z - 1);
        c = isSolid(x + 1, y + 1, z - 1);
        s2 = isSolid(x + 1, y + 1, z);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y + 1, z + 1);
        c = isSolid(x + 1, y + 1, z + 1);
        s2 = isSolid(x + 1, y + 1, z);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y + 1, z + 1);
        c = isSolid(x - 1, y + 1, z + 1);
        s2 = isSolid(x - 1, y + 1, z);
        ao[3] = calculateAO(s1, s2, c);
        break;
    case 3: // -Y
        s1 = isSolid(x, y - 1, z + 1);
        c = isSolid(x - 1, y - 1, z + 1);
        s2 = isSolid(x - 1, y - 1, z);
        ao[0] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y - 1, z + 1);
        c = isSolid(x + 1, y - 1, z + 1);
        s2 = isSolid(x + 1, y - 1, z);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y - 1, z - 1);
        c = isSolid(x + 1, y - 1, z - 1);
        s2 = isSolid(x + 1, y - 1, z);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x, y - 1, z - 1);
        c = isSolid(x - 1, y - 1, z - 1);
        s2 = isSolid(x - 1, y - 1, z);
        ao[3] = calculateAO(s1, s2, c);
        break;
    case 4: // +Z
        s1 = isSolid(x + 1, y, z + 1);
        c = isSolid(x + 1, y + 1, z + 1);
        s2 = isSolid(x, y + 1, z + 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y, z + 1);
        c = isSolid(x - 1, y + 1, z + 1);
        s2 = isSolid(x, y + 1, z + 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y, z + 1);
        c = isSolid(x - 1, y - 1, z + 1);
        s2 = isSolid(x, y - 1, z + 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y, z + 1);
        c = isSolid(x + 1, y - 1, z + 1);
        s2 = isSolid(x, y - 1, z + 1);
        ao[0] = calculateAO(s1, s2, c);
        break;
    case 5: // -Z
        s1 = isSolid(x - 1, y, z - 1);
        c = isSolid(x - 1, y + 1, z - 1);
        s2 = isSolid(x, y + 1, z - 1);
        ao[3] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y, z - 1);
        c = isSolid(x + 1, y + 1, z - 1);
        s2 = isSolid(x, y + 1, z - 1);
        ao[2] = calculateAO(s1, s2, c);
        s1 = isSolid(x + 1, y, z - 1);
        c = isSolid(x + 1, y - 1, z - 1);
        s2 = isSolid(x, y - 1, z - 1);
        ao[1] = calculateAO(s1, s2, c);
        s1 = isSolid(x - 1, y, z - 1);
        c = isSolid(x - 1, y - 1, z - 1);
        s2 = isSolid(x, y - 1, z - 1);
        ao[0] = calculateAO(s1, s2, c);
        break;
    }
}

void emitGreedyQuad(std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                    int normalId, int x, int y, int z, int width, int height, uint32_t textureId,
                    const uint8_t ao[4]) {

    auto baseIndex = static_cast<uint32_t>(vertices.size());
    uint64_t v0, v1, v2, v3;

    switch (normalId) {
    case 0: // +X: width=Z, height=Y
        v0 = packGreedyVertex(x + 1, y, z, normalId, 0, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x + 1, y, z + width, normalId, 1, textureId, ao[1], width, height);
        v2 = packGreedyVertex(x + 1, y + height, z + width, normalId, 2, textureId, ao[2], width,
                              height);
        v3 = packGreedyVertex(x + 1, y + height, z, normalId, 3, textureId, ao[3], width, height);
        break;
    case 1: // -X: width=Z, height=Y
        v0 = packGreedyVertex(x, y, z + width, normalId, 1, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x, y, z, normalId, 0, textureId, ao[1], width, height);
        v2 = packGreedyVertex(x, y + height, z, normalId, 3, textureId, ao[2], width, height);
        v3 = packGreedyVertex(x, y + height, z + width, normalId, 2, textureId, ao[3], width,
                              height);
        break;
    case 2: // +Y: width=X, height=Z
        v0 = packGreedyVertex(x, y + 1, z, normalId, 0, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x + width, y + 1, z, normalId, 1, textureId, ao[1], width, height);
        v2 = packGreedyVertex(x + width, y + 1, z + height, normalId, 2, textureId, ao[2], width,
                              height);
        v3 = packGreedyVertex(x, y + 1, z + height, normalId, 3, textureId, ao[3], width, height);
        break;
    case 3: // -Y: width=X, height=Z
        v0 = packGreedyVertex(x, y, z + height, normalId, 3, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x + width, y, z + height, normalId, 2, textureId, ao[1], width,
                              height);
        v2 = packGreedyVertex(x + width, y, z, normalId, 1, textureId, ao[2], width, height);
        v3 = packGreedyVertex(x, y, z, normalId, 0, textureId, ao[3], width, height);
        break;
    case 4: // +Z: width=X, height=Y
        v0 = packGreedyVertex(x + width, y, z + 1, normalId, 1, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x, y, z + 1, normalId, 0, textureId, ao[1], width, height);
        v2 = packGreedyVertex(x, y + height, z + 1, normalId, 3, textureId, ao[2], width, height);
        v3 = packGreedyVertex(x + width, y + height, z + 1, normalId, 2, textureId, ao[3], width,
                              height);
        break;
    case 5: // -Z: width=X, height=Y
        v0 = packGreedyVertex(x, y, z, normalId, 0, textureId, ao[0], width, height);
        v1 = packGreedyVertex(x + width, y, z, normalId, 1, textureId, ao[1], width, height);
        v2 = packGreedyVertex(x + width, y + height, z, normalId, 2, textureId, ao[2], width,
                              height);
        v3 = packGreedyVertex(x, y + height, z, normalId, 3, textureId, ao[3], width, height);
        break;
    }

    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);
    vertices.push_back(v3);

    // Diagonal flip based on AO to avoid artifacts
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 2);
    } else {
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 2);
    }
}

void processSliceGreedy(SliceMask& mask, std::vector<VoxelVertex>& vertices,
                        std::vector<uint32_t>& indices, int normalId, int slicePos, int sliceSize) {

    for (int v = 0; v < sliceSize; v++) {
        for (int u = 0; u < sliceSize;) {
            FaceData& face = mask[v][u];

            if (!face.isValid()) {
                u++;
                continue;
            }

            int width = 1;
            int height = 1;

            while (u + width < sliceSize && mask[v][u + width] == face) {
                width++;
            }

            bool canExtendV = true;
            while (canExtendV && v + height < sliceSize) {
                for (int du = 0; du < width; du++) {
                    if (!(mask[v + height][u + du] == face)) {
                        canExtendV = false;
                        break;
                    }
                }
                if (canExtendV)
                    height++;
            }

            int wx, wy, wz, quadWidth, quadHeight;

            switch (normalId) {
            case 0:
            case 1: // +X: u=Z, v=Y
                wx = slicePos;
                wz = u;
                wy = v;
                quadWidth = width;
                quadHeight = height;
                break;
            case 2:
            case 3: // +Y: u=X, v=Z
                wx = u;
                wy = slicePos;
                wz = v;
                quadWidth = width;
                quadHeight = height;
                break;
            case 4:
            case 5: // +Z: u=X, v=Y
            default:
                wx = u;
                wy = v;
                wz = slicePos;
                quadWidth = width;
                quadHeight = height;
                break;
            }

            emitGreedyQuad(vertices, indices, normalId, wx, wy, wz, quadWidth, quadHeight,
                           face.textureId, face.ao);

            for (int dv = 0; dv < height; dv++) {
                for (int du = 0; du < width; du++) {
                    mask[v + dv][u + du].blockId = -1;
                }
            }

            u += width;
        }
    }
}

} // namespace

void ChunkMesh::generateMesh(const Chunk& mainChunk, const BlockRegistry& registry,
                             std::vector<VoxelVertex>& vertices, std::vector<uint32_t>& indices,
                             const Chunk* neighborNorth, const Chunk* neighborSouth,
                             const Chunk* neighborEast, const Chunk* neighborWest,
                             const Chunk* neighborTop, const Chunk* neighborBottom,
                             const std::vector<uint32_t>& textureCache) {
    ZoneScoped;
    vertices.clear();
    indices.clear();

    if (mainChunk.isEmpty())
        return;

    vertices.reserve(3000);
    indices.reserve(4500);

    const int SIZE = Chunk::CHUNK_SIZE;
    int faceCount = 0;

    SliceMask mask;

    {
        ZoneScopedN("Greedy Mesh All Faces");

        // +X faces
        for (int sliceX = 0; sliceX < SIZE; sliceX++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int y = 0; y < SIZE; y++) {
                for (int z = 0; z < SIZE; z++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(sliceX, y, z));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(sliceX + 1, y, z, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[y][z];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 2, textureCache);
                        calculateAOForFace(mainChunk, registry, sliceX, y, z, 0, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 0, sliceX, SIZE);
        }

        // -X faces
        for (int sliceX = 0; sliceX < SIZE; sliceX++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int y = 0; y < SIZE; y++) {
                for (int z = 0; z < SIZE; z++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(sliceX, y, z));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(sliceX - 1, y, z, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[y][z];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 2, textureCache);
                        calculateAOForFace(mainChunk, registry, sliceX, y, z, 1, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 1, sliceX, SIZE);
        }

        // +Y faces
        for (int sliceY = 0; sliceY < SIZE; sliceY++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int z = 0; z < SIZE; z++) {
                for (int x = 0; x < SIZE; x++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(x, sliceY, z));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(x, sliceY + 1, z, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[z][x];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 0, textureCache);
                        calculateAOForFace(mainChunk, registry, x, sliceY, z, 2, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 2, sliceY, SIZE);
        }

        // -Y faces
        for (int sliceY = 0; sliceY < SIZE; sliceY++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int z = 0; z < SIZE; z++) {
                for (int x = 0; x < SIZE; x++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(x, sliceY, z));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(x, sliceY - 1, z, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[z][x];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 1, textureCache);
                        calculateAOForFace(mainChunk, registry, x, sliceY, z, 3, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 3, sliceY, SIZE);
        }

        // +Z faces
        for (int sliceZ = 0; sliceZ < SIZE; sliceZ++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int y = 0; y < SIZE; y++) {
                for (int x = 0; x < SIZE; x++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(x, y, sliceZ));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(x, y, sliceZ + 1, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[y][x];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 2, textureCache);
                        calculateAOForFace(mainChunk, registry, x, y, sliceZ, 4, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 4, sliceZ, SIZE);
        }

        // -Z faces
        for (int sliceZ = 0; sliceZ < SIZE; sliceZ++) {
            for (auto& row : mask)
                for (auto& cell : row)
                    cell.blockId = -1;

            for (int y = 0; y < SIZE; y++) {
                for (int x = 0; x < SIZE; x++) {
                    int blockId = static_cast<int>(mainChunk.getBlockUnsafe(x, y, sliceZ));
                    if (blockId == 0 || !registry.isDisplayable(blockId))
                        continue;

                    bool isNeighborSolid = mainChunk.isPaddedBlockSolid(x, y, sliceZ - 1, registry);

                    if (!isNeighborSolid) {
                        FaceData& face = mask[y][x];
                        face.blockId = blockId;
                        face.textureId = getTextureForFace(blockId, 2, textureCache);
                        calculateAOForFace(mainChunk, registry, x, y, sliceZ, 5, face.ao);
                        faceCount++;
                    }
                }
            }
            processSliceGreedy(mask, vertices, indices, 5, sliceZ, SIZE);
        }
    }

    int mergedQuadCount = static_cast<int>(vertices.size() / 4);
    TracyPlot("Original Faces", static_cast<int64_t>(faceCount));
    TracyPlot("Merged Quads", static_cast<int64_t>(mergedQuadCount));
    TracyPlot("Greedy Reduction %",
              faceCount > 0 ? static_cast<int64_t>(100 - (mergedQuadCount * 100 / faceCount)) : 0);
}

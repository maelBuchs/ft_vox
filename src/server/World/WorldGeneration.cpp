#include <cmath>
#include <cstdlib>
#include <string>

#include <tracy/Tracy.hpp>

#include "common/World/Chunk.hpp"
#include "WorldManager.hpp"

enum BlockType : uint8_t {
    kAIR = 0,
    kSTONE = 1,
    kGRASS = 2,
    kDIRT = 3,
    kOAK = 4,
    kBEDROCK = 5,
    kWATER = 6,
    kSAND = 7,
    kCAVE_AIR = 8
};
namespace {

int getHeightValue(int bx, int bz, float continentalness, BiomeType biome,
                   tk::spline& heightSpline) {

    float baseHeight = static_cast<float>(heightSpline(continentalness * 2.0F));
    return baseHeight;
}

/*
 * @brief Adds surface layers to the given chunk based on height map and biome data.
 *
 * This function modifies the provided chunk by adding surface blocks according to
 * the pre-computed height map and biome information. It sets different block IDs
 * for the top layers based on the biome type at each (bx, bz) coordinate.
 *
 * @param chunk Pointer to the Chunk object to be modified.
 * @param heightMap A 2D array representing the height values for each (bx, bz) coordinate.
 */
void addSurface(Chunk* chunk, const int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE]) {
    ZoneScopedN("addSurface");
    // Simple surface addition: set the top block to a different ID (e.g., grass)
    // Use pre-computed heightMap to avoid redundant getHeightValue() calls
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {

            int maxHeight = heightMap[bx][bz];

            if (maxHeight >= chunkToWorld(0, chunk->getPosition()[1]) &&
                maxHeight < chunkToWorld(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, maxHeight % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 1 >= chunkToWorld(0, chunk->getPosition()[1]) &&
                maxHeight + 1 < chunkToWorld(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 2 >= chunkToWorld(0, chunk->getPosition()[1]) &&
                maxHeight + 2 < chunkToWorld(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 3 >= chunkToWorld(0, chunk->getPosition()[1]) &&
                maxHeight + 3 < chunkToWorld(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                auto biome = chunk->getBiomeDataAt(bx, bz);
                if (biome == BiomeType::kPLAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kGRASS);
                } else if (biome == BiomeType::kMOUNTAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSTONE);
                } else {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSAND);
                }
            }

            if (chunk->getPosition()[1] == 0) {
                chunk->setBlock(bx, 0, bz, kBEDROCK);
            }
        }
    }
}

void addCaves(Chunk* chunk, int64_t seed) {
    // Simple cave generation using 3D Perlin noise
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                int worldX = chunkToWorld(bx, chunk->getPosition()[0]);
                int worldY = chunkToWorld(by, chunk->getPosition()[1]);
                int worldZ = chunkToWorld(bz, chunk->getPosition()[2]);

                float caveNoise = perlinNoise3D(worldX, worldY, worldZ, noise_config::kCAVE, seed);

                // Threshold to determine if block is part of a cave
                // if (caveNoise < 0.0F) {
                if (caveNoise > 0.2F && caveNoise < 0.25F) {
                    auto block = chunk->getBlock(bx, by, bz);
                    if (block != kBEDROCK && block != kWATER)
                        chunk->setBlock(bx, by, bz, kAIR); // Set block to air
                }
            }
        }
    }
}
} // namespace
std::shared_ptr<Chunk> WorldManager::generateChunk(const glm::ivec3& pos) {
    ZoneScopedN("generateChunk");
    ZoneValue(pos[0]);
    ZoneText(("Chunk(" + std::to_string(pos[0]) + "," + std::to_string(pos[1]) + "," +
              std::to_string(pos[2]) + ")")
                 .c_str(),
             32);

    std::shared_ptr<Chunk> chunk;
    {
        ZoneScopedN("Chunk allocation");
        chunk = std::make_shared<Chunk>(pos[0], pos[1], pos[2], *this);
    }

    if (pos[1] > 8 || pos[1] < 0) {
        return chunk; // Return empty chunk for Y < 0
    }
    int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE];

    {
        ZoneScopedN("HeightMap noise sampling");
        for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                heightMap[bx][bz] =
                    getHeightValue(chunkToWorld(bx, pos[0]), chunkToWorld(bz, pos[2]),
                                   chunk->getNoise(bx, bz, NoiseType::kCONTINENT),
                                   chunk->getBiomeDataAt(bx, bz), getHeightSpline());
            }
        }
    }

    {
        ZoneScopedN("Surface layer placement");
        for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                int maxHeight = heightMap[bx][bz];
                /* Under-surface fill */
                if (maxHeight >= chunkToWorld(0, pos[1]) &&
                    maxHeight < chunkToWorld(Chunk::CHUNK_SIZE, pos[1])) {
                    chunk->setBlock(bx, maxHeight % Chunk::CHUNK_SIZE, bz, kDIRT);
                }
                if (maxHeight + 1 >= chunkToWorld(0, pos[1]) &&
                    maxHeight + 1 < chunkToWorld(Chunk::CHUNK_SIZE, pos[1])) {
                    chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, kDIRT);
                }
                if (maxHeight + 2 >= chunkToWorld(0, pos[1]) &&
                    maxHeight + 2 < chunkToWorld(Chunk::CHUNK_SIZE, pos[1])) {
                    chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, kDIRT);
                }
                /* Surface block based on biome */
                if (maxHeight + 3 >= chunkToWorld(0, pos[1]) &&
                    maxHeight + 3 < chunkToWorld(Chunk::CHUNK_SIZE, pos[1])) {
                    auto biome = chunk->getBiomeDataAt(bx, bz);
                    if (biome == BiomeType::kPLAINS) {
                        chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kGRASS);
                    } else if (biome == BiomeType::kMOUNTAINS) {
                        chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSTONE);
                    } else {
                        chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSAND);
                    }
                }
                // debug to see chunk borders
                // if (bx == 0 || bz == 0) {
                //     chunk->setBlock(bx, 0, bz, kBEDROCK);
                // }
                // if (bx == Chunk::CHUNK_SIZE - 1 || bz == Chunk::CHUNK_SIZE - 1) {
                //     chunk->setBlock(bx, 0, bz, kBEDROCK);
                // }
            }
        }
    }
    {
        ZoneScopedN("Stone fill below surface");
        for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                int maxHeight = heightMap[bx][bz];
                for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                    if (chunkToWorld(by, pos[1]) < maxHeight) {
                        chunk->setBlock(bx, by, bz, kSTONE);
                    }
                }
            }
        }
    }

    {
        ZoneScopedN("Water placement");
        for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                    auto block = chunk->getBlock(bx, by, bz);
                    if (chunkToWorld(by, pos[1]) < 60 && block == kAIR) {
                        chunk->setBlock(bx, by, bz, kWATER);
                    }
                    if (block == kWATER) {
                        chunk->setBlock(bx, by, bz, kOAK);
                    }
                }
            }
        }
    }

    {
        ZoneScopedN("Cave generation (3D noise)");
        for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
            for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                    auto block = chunk->getBlock(bx, by, bz);
                    float caveNoise =
                        perlinNoise3D(chunkToWorld(bx, pos[0]), chunkToWorld(by, pos[1]),
                                      chunkToWorld(bz, pos[2]), noise_config::kCAVE, kSEED);

                    /* Spaghetti cave generation */
                    if ((caveNoise > 0.0F && caveNoise < 0.2F)) {
                        if (block != kBEDROCK && block != kWATER && block != kGRASS &&
                            block != kSAND && block != kAIR) {
                            chunk->setBlock(bx, by, bz, kCAVE_AIR);
                        }
                    }
                    /* Cheese cave generation */
                    if (caveNoise > 0.8F) {
                        if (block != kBEDROCK && block != kWATER) {
                            chunk->setBlock(bx, by, bz, kCAVE_AIR);
                        }
                    }
                }
            }
        }
    }

    {
        ZoneScopedN("Bedrock placement");
        if (pos[1] == 0) {
            for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
                for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
                    chunk->setBlock(bx, 0, bz, kBEDROCK);
                }
            }
        }
    }

    return chunk;
}

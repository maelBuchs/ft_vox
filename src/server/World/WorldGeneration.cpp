#include <cmath>
#include <cstdlib>

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
    // Simple surface addition: set the top block to a different ID (e.g., grass)
    // Use pre-computed heightMap to avoid redundant getHeightValue() calls
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {

            int maxHeight = heightMap[bx][bz];

            if (maxHeight >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, maxHeight % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 1 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 1 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 2 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 2 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 3 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 3 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
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
                int worldX = CHUNK_TO_WORLD(bx, chunk->getPosition()[0]);
                int worldY = CHUNK_TO_WORLD(by, chunk->getPosition()[1]);
                int worldZ = CHUNK_TO_WORLD(bz, chunk->getPosition()[2]);

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
    ZoneScoped;

    auto chunk = std::make_shared<Chunk>(pos[0], pos[1], pos[2], *this);

    if (pos[1] > 8 || pos[1] < 0) {
        return chunk; // Return empty chunk for Y < 0
    }
    int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE];

    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            heightMap[bx][bz] =
                getHeightValue(CHUNK_TO_WORLD(bx, pos[0]), CHUNK_TO_WORLD(bz, pos[2]),
                               chunk->getNoise(bx, bz, NoiseType::kCONTINENT),
                               chunk->getBiomeDataAt(bx, bz), getHeightSpline());
            int maxHeight = heightMap[bx][bz];
            /* Under-surface fill */
            // TODO - Cleaner way to do this?
            if (maxHeight >= CHUNK_TO_WORLD(0, pos[1]) &&
                maxHeight < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, pos[1])) {
                chunk->setBlock(bx, maxHeight % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 1 >= CHUNK_TO_WORLD(0, pos[1]) &&
                maxHeight + 1 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, pos[1])) {
                chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            if (maxHeight + 2 >= CHUNK_TO_WORLD(0, pos[1]) &&
                maxHeight + 2 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, pos[1])) {
                chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, kDIRT);
            }
            /* Surface block based on biome */
            if (maxHeight + 3 >= CHUNK_TO_WORLD(0, pos[1]) &&
                maxHeight + 3 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, pos[1])) {
                auto biome = chunk->getBiomeDataAt(bx, bz);
                if (biome == BiomeType::kPLAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kGRASS);
                } else if (biome == BiomeType::kMOUNTAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSTONE);
                } else {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, kSAND);
                }
            }

            for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                auto block = chunk->getBlock(bx, by, bz);
                if (CHUNK_TO_WORLD(by, pos[1]) < maxHeight) {
                    chunk->setBlock(bx, by, bz, kSTONE);
                    block = kSTONE; // Set block ID to 1 (solid block)
                }
                if (CHUNK_TO_WORLD(by, pos[1]) < 60 && block == 0) {
                    chunk->setBlock(bx, by, bz, kWATER); // Set block ID to 6 (water block)
                    block = kWATER;
                    continue;
                }
                if (block == kWATER) {
                    chunk->setBlock(bx, by, bz, kOAK);
                }
                float caveNoise =
                    perlinNoise3D(CHUNK_TO_WORLD(bx, pos[0]), CHUNK_TO_WORLD(by, pos[1]),
                                  CHUNK_TO_WORLD(bz, pos[2]), noise_config::kCAVE, kSEED);

                /* Spaghetti ave generation */
                if ((caveNoise > 0.0F && caveNoise < 0.2F)) {
                    if (block != kBEDROCK && block != kWATER && block != kGRASS && block != kSAND) {
                        chunk->setBlock(bx, by, bz, kCAVE_AIR); // Set block to cave air
                        block = kCAVE_AIR;
                    }
                }
                /* Cheese cave generation */
                if (caveNoise > 0.8F) {
                    if (block != kBEDROCK && block != kWATER) {
                        chunk->setBlock(bx, by, bz, kCAVE_AIR); // Set block to cave air
                        block = kCAVE_AIR;
                    }
                }
            }

            if (pos[1] == 0) {
                chunk->setBlock(bx, 0, bz, 5);
            }
        }
    }
    return chunk;
}

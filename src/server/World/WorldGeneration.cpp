#include <cmath>
#include <cstdlib>

#include <tracy/Tracy.hpp>

#include "common/World/Chunk.hpp"
#include "WorldManager.hpp"
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
                chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 2 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 2 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 3 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 3 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                auto biome = chunk->getBiomeDataAt(bx, bz);
                if (biome == BiomeType::kPLAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 2);
                } else if (biome == BiomeType::kMOUNTAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 1);
                } else if (biome == BiomeType::kOCEAN) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 7);
                } else if (biome == BiomeType::kNONE) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 7);
                } else {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 7);
                }
            }

            if (chunk->getPosition()[1] == 0) {
                chunk->setBlock(bx, 0, bz, 5);
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
                    if (block != 5 && block != 6)
                        chunk->setBlock(bx, by, bz, 0); // Set block to air
                }
            }
        }
    }
}
} // namespace
std::shared_ptr<Chunk> WorldManager::generateChunk(const glm::ivec3& pos) {
    ZoneScoped;

    auto chunk = std::make_shared<Chunk>(pos[0], pos[1], pos[2], *this);
    // Pre-compute height map once to avoid redundant calculations
    // This cuts height calculations in half (was called in both generateChunk and addSurface)
    int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE];

    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            heightMap[bx][bz] =
                getHeightValue(CHUNK_TO_WORLD(bx, pos[0]), CHUNK_TO_WORLD(bz, pos[2]),
                               chunk->getNoise(bx, bz, NoiseType::kCONTINENT),
                               chunk->getBiomeDataAt(bx, bz), getHeightSpline());
        }
    }

    // Fill chunk with terrain using pre-computed height map
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            int maxHeight = heightMap[bx][bz];
            for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, pos[1]) < maxHeight) {
                    chunk->setBlock(bx, by, bz, 1); // Set block ID to 1 (solid block)
                }
            }
        }
    }
    addSurface(chunk.get(), heightMap);

    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            // int maxHeight = heightMap[bx][bz];
            for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, pos[1]) < 60 && chunk->getBlock(bx, by, bz) == 0) {
                    chunk->setBlock(bx, by, bz, 6); // Set block ID to 6 (water block)
                }
            }
        }
    }
    addCaves(chunk.get(), getSeed());
    // Add surface layers using same height map
    return chunk;
}

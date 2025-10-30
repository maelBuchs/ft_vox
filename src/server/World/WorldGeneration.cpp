#include <cmath>
#include <cstdlib>

#include "common/World/Chunk.hpp"
#include "WorldManager.hpp"
namespace {

int getHeightValue(int bx, int bz) {
    float bxF = static_cast<float>(bx);
    float bzF = static_cast<float>(bz);

    const float frequency = 0.02F;

    float noiseValue = perlinValue(bxF * frequency, bzF * frequency * 0.1, SEED);
    noiseValue += 0.5F * perlinValue(bxF * frequency * 2.0F, bzF * frequency * 2.0F, SEED);
    noiseValue += 0.25F * perlinValue(bxF * frequency * 4.0F, bzF * frequency * 4.0F, SEED);
    noiseValue /= 1.0F + 0.5F + 0.25F;
    const float min_height = 0.0F;
    const float max_height = 280.0F;
    noiseValue = std::pow(noiseValue, 0.8F);
    // int height = static_cast<int>(min_height + noiseValue * (max_height - min_height));
    // int height = static_cast<int>(noiseValue);
    int height = static_cast<int>(min_height + (noiseValue * (max_height - min_height)));

    return height;
}

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
                chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 2);
            }
            if (chunk->getPosition()[1] == 0) {
                chunk->setBlock(bx, 0, bz, 5);
            }
        }
    }
}
} // namespace
std::shared_ptr<Chunk> WorldManager::generateChunk(const glm::ivec3& pos) {
    auto chunk = std::make_shared<Chunk>(pos[0], pos[1], pos[2]);

    // Pre-compute height map once to avoid redundant calculations
    // This cuts height calculations in half (was called in both generateChunk and addSurface)
    int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE];

    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            heightMap[bx][bz] = getHeightValue(CHUNK_TO_WORLD(bx, pos[0]), CHUNK_TO_WORLD(bz, pos[2]));
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

    // Add surface layers using same height map
    addSurface(chunk.get(), heightMap);
    return chunk;
}

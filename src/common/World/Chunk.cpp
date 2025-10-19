#include "Chunk.hpp"

#include <cmath>
#include <iostream>

#include <glm/glm.hpp>

#include "common/Util/perlinNoise.hpp"
#define CHUNK_TO_WORLD(b, c) ((c) * Chunk::CHUNK_SIZE + (b))
#define SEED 42L

Chunk::Chunk(int x, int y, int z) : _blocks{} {
    _blocks.fill(AIR_BLOCK_ID);

    // All chunks are at y=0 for this test
    std::tuple<int, int, int> pos = {x, y, z};
    position = pos;

    // Create a new chunk and generate its block data
    // Fill with some blocks for testing (staircase-like pattern)
    for (int bx = 0; bx < CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < CHUNK_SIZE; bz++) {
            float noiseValue = perlinNoiseByCoordinates(CHUNK_TO_WORLD(bx, std::get<0>(position)),
                                                        CHUNK_TO_WORLD(bz, std::get<2>(position)),
                                                        0.01F, SEED, 1, 0.1F);

            int maxHeight =
                static_cast<int>((noiseValue + 1.0F) / 2.0F * static_cast<float>(CHUNK_SIZE));
            for (int by = 0; by < CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, y) < maxHeight) {
                    setBlock(bx, by, bz, 2); // Set block ID to 1 (solid block)
                }
            }
        }
    }
}

uint8_t Chunk::getBlock(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return AIR_BLOCK_ID;
    }
    return _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z)));
}

void Chunk::setBlock(int x, int y, int z, uint8_t blockId) {
    if (!isInBounds(x, y, z)) {
        return;
    }
    _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z))) = blockId;
    if (blockId != AIR_BLOCK_ID) {
        _isEmpty = false;
    }
}

bool Chunk::isBlockSolid(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return false;
    }
    return getBlock(x, y, z) != AIR_BLOCK_ID;
}

bool Chunk::isInBounds(int x, int y, int z) const {
    return x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
}

int Chunk::getIndex(int x, int y, int z) const {
    return x + (y * CHUNK_SIZE) + (z * CHUNK_SIZE * CHUNK_SIZE);
}

void ChunkInstanciator::loadChunkAt(int x, int y, int z) {
    decltype(_loadedChunks)::key_type key(x, y, z);
    if (_loadedChunks.contains(key)) {
        return; // Chunk already loaded
    }
    _loadedChunks[key] = std::make_unique<Chunk>(x, y, z);
}
// void ChunkInstanciator::unloadChunkAt(int x, int y, int z) {}

void ChunkInstanciator::updateChunksAroundPlayer(float playerX, float playerY, float playerZ,
                                                 float viewDistance) {
    int cxmin = static_cast<int>(std::floor((playerX / Chunk::CHUNK_SIZE) - viewDistance));
    int cxmax = static_cast<int>(std::floor((playerX / Chunk::CHUNK_SIZE) + viewDistance));
    int cymin = static_cast<int>(std::floor((playerY / Chunk::CHUNK_SIZE) - viewDistance));
    int cymax = static_cast<int>(std::floor((playerY / Chunk::CHUNK_SIZE) + viewDistance));
    int czmin = static_cast<int>(std::floor((playerZ / Chunk::CHUNK_SIZE) - viewDistance));
    int czmax = static_cast<int>(std::floor((playerZ / Chunk::CHUNK_SIZE) + viewDistance));
    for (int x = cxmin; x <= cxmax; x++) {
        for (int y = cymin; y <= cymax; y++) {
            for (int z = czmin; z <= czmax; z++) {
                loadChunkAt(x, y, z);
            }
        }
    }
}

// hecks which chunks need to be loaded/unloaded based on player position

#include "Chunk.hpp"

#include <cmath>

#include <glm/glm.hpp>
#define RENDER_DISTANCE 32

Chunk::Chunk(int x, int y, int z) : _blocks{} {
    _blocks.fill(AIR_BLOCK_ID);

    // All chunks are at y=0 for this test
    std::tuple<int, int, int> pos = {x, 0, z};

    // Create a new chunk and generate its block data
    // Fill with some blocks for testing (staircase-like pattern)
    for (int bx = 0; bx < 32; bx++) {
        for (int bz = 0; bz < 32; bz++) {
            int height = (bx + bz) / 2;
            for (int by = 0; by < height && by < 32; by++) {
                if (by < height - 5) {
                    setBlock(bx, by, bz, 1); // stone
                } else if (by < height - 1) {
                    setBlock(bx, by, bz, 2); // grass_block
                } else if (by < height && (bx % 3 == 0) && (bz % 3 == 0)) {
                    setBlock(bx, by, bz, 4); // water
                } else if (by < height) {
                    setBlock(bx, by, bz, 3); // oak_wood (some trees)
                } else {
                    setBlock(bx, by, bz, 0); // air
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
    decltype(_loadedChunks)::key_type key = decltype(_loadedChunks)::key_type(x, y, z);
    if (_loadedChunks.contains(key)) {
        return; // Chunk already loaded
    }
    _loadedChunks[key] = std::make_unique<Chunk>(x, y, z);
}
// void ChunkInstanciator::unloadChunkAt(int x, int y, int z) {}

void ChunkInstanciator::updateChunksAroundPlayer(float playerX, float playerY, float playerZ,
                                                 float viewDistance) {
    int cxmin = static_cast<int>(std::floor((playerX - viewDistance) / Chunk::CHUNK_SIZE));
    int cxmax = static_cast<int>(std::floor((playerX + viewDistance) / Chunk::CHUNK_SIZE));
    int cymin = static_cast<int>(std::floor((playerY - viewDistance) / Chunk::CHUNK_SIZE));
    int cymax = static_cast<int>(std::floor((playerY + viewDistance) / Chunk::CHUNK_SIZE));
    int czmin = static_cast<int>(std::floor((playerZ - viewDistance) / Chunk::CHUNK_SIZE));
    int czmax = static_cast<int>(std::floor((playerZ + viewDistance) / Chunk::CHUNK_SIZE));
    for (int x = cxmin; x <= cxmax; x++) {
        for (int y = cymin; y <= cymax; y++) {
            for (int z = czmin; z <= czmax; z++) {
                loadChunkAt(x, y, z);
            }
        }
    }
}

// hecks which chunks need to be loaded/unloaded based on player position

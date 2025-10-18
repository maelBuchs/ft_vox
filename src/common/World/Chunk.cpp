#include "Chunk.hpp"

#include <cmath>

Chunk::Chunk(int x, int y, int z) : _blocks{} {
    _blocks.fill(AIR_BLOCK_ID);

    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int y = 0; y < 3; y++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                setBlock(x, y, z, 1);
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
    if (_loadedChunks.find(std::make_tuple(x, y, z)) != _loadedChunks.end()) {
        return; // Chunk already loaded
    }
    _loadedChunks[std::make_tuple(x, y, z)] = std::make_unique<Chunk>(x, y, z);
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

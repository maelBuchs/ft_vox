#include "Chunk.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <sys/types.h>
#include <vector>

#include "common/Util/Util.hpp"
#include "glm/detail/qualifier.hpp"
#include "glm/fwd.hpp"

#define NB_ADD_LAND 5

using Matrix = std::vector<std::vector<uint32_t>>;

std::vector<uint8_t> matrixToBiomeData(const Matrix& biomeMap) {
    int width = biomeMap.size();
    int height = biomeMap[0].size();
    std::vector<uint8_t> biomeData;
    biomeData.reserve(width * height);

    for (int x = 0; x < width; ++x) {
        for (int z = 0; z < height; ++z) {
            biomeData.push_back(static_cast<uint8_t>(biomeMap[x][z]));
        }
    }

    return biomeData;
}

std::vector<BiomeType> determineBiome(int x, int z, Chunk& chunk) {
    std::vector<BiomeType> biomeData;
    biomeData.reserve(Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE);

    for (int localX = 0; localX < Chunk::CHUNK_SIZE; ++localX) {
        for (int localZ = 0; localZ < Chunk::CHUNK_SIZE; ++localZ) {
            int worldX = CHUNK_TO_WORLD(localX, chunk.getPosition().x);
            int worldZ = CHUNK_TO_WORLD(localZ, chunk.getPosition().z);

            float temperature = perlinNoise(worldX, worldZ, NoiseConfig::TEMPERATURE);

            float humidity = perlinNoise(worldX, worldZ, NoiseConfig::HUMIDITY);

            BiomeType biome;
            if (temperature < 0.3f) {
                biome = BiomeType::OCEAN;
            } else if (temperature < 0.6f) {
                biome = BiomeType::PLAINS;
            } else {
                biome = BiomeType::MOUNTAINS;
            }

            biomeData.push_back(biome);
        }
    }

    return biomeData;
}

Chunk::Chunk(int x, int y, int z) : position{x, y, z}, _blocks{}, _isEmpty(false) {
    _blocks.fill(AIR_BLOCK_ID);

    _biomeData = determineBiome(x, z, *this);
    // if (!loadChunk()) {
    // generateChunk();
    // }
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

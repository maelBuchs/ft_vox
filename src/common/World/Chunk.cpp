#include "Chunk.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <sys/types.h>
#include <vector>

#include "common/Util/Util.hpp"
#include "glm/detail/qualifier.hpp"
#include "glm/fwd.hpp"
#include "server/World/WorldManager.hpp"

#define NB_ADD_LAND 5

using Matrix = std::vector<std::vector<uint32_t>>;
namespace {
// std::vector<uint8_t> matrixToBiomeData(const Matrix& biomeMap) {
//     int width = biomeMap.size();
//     int height = biomeMap[0].size();
//     std::vector<uint8_t> biomeData;
//     biomeData.reserve(width * height);

//     for (int x = 0; x < width; ++x) {
//         for (int z = 0; z < height; ++z) {
//             biomeData.push_back(static_cast<uint8_t>(biomeMap[x][z]));
//         }
//     }

//     return biomeData;
// }

std::vector<BiomeType> determineBiome(Chunk& chunk, int64_t seed) {
    std::vector<BiomeType> biomeData;
    biomeData.reserve(static_cast<long>(Chunk::CHUNK_SIZE) * Chunk::CHUNK_SIZE);

    for (int localX = 0; localX < Chunk::CHUNK_SIZE; ++localX) {
        for (int localZ = 0; localZ < Chunk::CHUNK_SIZE; ++localZ) {
            int worldX = CHUNK_TO_WORLD(localX, chunk.getPosition()[0]);
            int worldZ = CHUNK_TO_WORLD(localZ, chunk.getPosition()[2]);

            float continent = perlinNoise(worldX, worldZ, noise_config::kCONTINENT, seed);
            float humidity = perlinNoise(worldX, worldZ, noise_config::kHUMIDITY, seed);

            BiomeType biome = BiomeType::kNONE;
            if (continent < 0) {
                biome = BiomeType::kNONE;
            } else if (continent > 0.38F) {
                biome = BiomeType::kMOUNTAINS;
            } else {
                biome = BiomeType::kPLAINS;
            }

            biomeData.push_back(biome);
        }
    }

    return biomeData;
}
} // namespace

Chunk::Chunk(int x, int y, int z, WorldManager& worldManager)
    : position{x, y, z}, _isEmpty(false), kSEED(worldManager.getSeed()) {
    _blocks.fill(0);

    _biomeData = determineBiome(*this, kSEED);
    // if (!loadChunk()) {
    // generateChunk();
    // }
}

uint8_t Chunk::getBlock(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return 0;
    }
    return _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z)));
}

void Chunk::setBlock(int x, int y, int z, uint8_t blockId) {
    if (!isInBounds(x, y, z)) {
        return;
    }
    _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z))) = blockId;
    if (blockId != 0) {
        _isEmpty = false;
    }
}

bool Chunk::isBlockSolid(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return false;
    }
    return getBlock(x, y, z) != 0;
}

bool Chunk::isInBounds(int x, int y, int z) {
    return x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
}

int Chunk::getIndex(int x, int y, int z) {
    return x + (y * CHUNK_SIZE) + (z * CHUNK_SIZE * CHUNK_SIZE);
}

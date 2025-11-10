#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "server/World/WorldManager.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#ifndef SEED
#define SEED 42L
#endif
#include "../Util/Util.hpp"
#include "common/World/ChunkMesh.hpp"
#include "glm/fwd.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#define RENDER_DISTANCE 6
#define MAX_THREADS 2
#define CHUNK_TO_WORLD(b, c) (((c) * Chunk::CHUNK_SIZE) + (b))

enum class BiomeType : uint8_t { OCEAN, PLAINS, MOUNTAINS, NONE };
enum class NoiseType { TEMPERATURE, HUMIDITY, CONTINENT, EROSION, WEIRDNESS, DEPTH };
class Chunk;
using chunkMap = std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>>;

using ChunkCoord = glm::ivec3;

class Chunk {
  public:
    static constexpr int CHUNK_SIZE = 32;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    static constexpr uint8_t AIR_BLOCK_ID = 0;

    Chunk(int x, int y, int z);
    Chunk() = default;
    ~Chunk() = default;

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = default;
    Chunk& operator=(Chunk&&) = default;

    // Block access (using block IDs from BlockRegistry)
    [[nodiscard]] uint8_t getBlock(int x, int y, int z) const;

    // Unsafe but fast - skips bounds checking (caller must guarantee x,y,z in [0,CHUNK_SIZE))
    [[nodiscard]] inline uint8_t getBlockUnsafe(int x, int y, int z) const {
        return _blocks[x + (y * CHUNK_SIZE) + (z * CHUNK_SIZE * CHUNK_SIZE)];
    }

    void setBlock(int x, int y, int z, uint8_t blockId);

    // Helper functions
    [[nodiscard]] bool isBlockSolid(int x, int y, int z) const;
    [[nodiscard]] bool isInBounds(int x, int y, int z) const;
    [[nodiscard]] int getIndex(int x, int y, int z) const;

    [[nodiscard]] bool isEmpty() const { return _isEmpty; }
    void setEmpty(bool empty) { _isEmpty = empty; }

    // TODO - Cleanup this crap
    glm::ivec3 getPosition() const { return position; }
    [[nodiscard]] std::string getBiome(glm::ivec3 pos) const {
        if (_biomeData.size() < 1) {
            return "N/A";
        }
        switch (static_cast<BiomeType>(_biomeData[((pos.x * CHUNK_SIZE) + pos.z)])) {
        case BiomeType::PLAINS:
            return "Plains";
        case BiomeType::MOUNTAINS:
            return "Mountains";
        case BiomeType::OCEAN:
            return "Ocean";
        default:
            return "Unknown";
        }
    }
    [[nodiscard]] float getNoise(int x, int z, NoiseType type) const {
        int bx = CHUNK_TO_WORLD(x, position[0]);
        int bz = CHUNK_TO_WORLD(z, position[2]);
        switch (type) {
        case NoiseType::TEMPERATURE:
            return perlinNoise(bx, bz, NoiseConfig::TEMPERATURE);
        case NoiseType::HUMIDITY:
            return perlinNoise(bx, bz, NoiseConfig::HUMIDITY);
        case NoiseType::CONTINENT:
            return perlinNoise(bx, bz, NoiseConfig::CONTINENT);
        case NoiseType::EROSION:
            return perlinNoise(bx, bz, NoiseConfig::EROSION);
        case NoiseType::WEIRDNESS:
            return perlinNoise(bx, bz, NoiseConfig::WEIRDNESS);
        case NoiseType::DEPTH:
            return perlinNoise(bx, bz, NoiseConfig::DEPTH);
        default:
            return 0.0F;
        }
    }
    [[nodiscard]] BiomeParams getNoiseParams(int x, int z) const {
        int bx = CHUNK_TO_WORLD(x, position[0]);
        int bz = CHUNK_TO_WORLD(z, position[2]);
        BiomeParams biomeParams{};
        biomeParams.temperature = perlinNoise(bx, bz, NoiseConfig::TEMPERATURE);
        biomeParams.humidity = perlinNoise(bx, bz, NoiseConfig::HUMIDITY);
        biomeParams.continent = perlinNoise(bx, bz, NoiseConfig::CONTINENT);
        biomeParams.erosion = perlinNoise(bx, bz, NoiseConfig::EROSION);
        biomeParams.weirdness = perlinNoise(bx, bz, NoiseConfig::WEIRDNESS);
        biomeParams.depth = perlinNoise(bx, bz, NoiseConfig::DEPTH);
        return biomeParams;
    }
    BiomeType getBiomeDataAt(int x, int z) const {
        int width = static_cast<int>(std::sqrt(_biomeData.size()));
        if (x < 0 || x >= width || z < 0 || z >= width) {
            return BiomeType::NONE;
        }
        return _biomeData[(x * width + z)];
    }

  private:
    glm::ivec3 position;
    std::array<uint8_t, VOLUME> _blocks;
    bool _isEmpty = true;
    int8_t _biome = static_cast<int8_t>(BiomeType::PLAINS);
    std::vector<BiomeType> _biomeData;
    void generateChunk();
    bool loadChunk();
};

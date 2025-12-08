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

#include "../Util/Util.hpp"
#include "common/World/ChunkMesh.hpp"
#include "glm/fwd.hpp"
#include "server//World/WorldManager.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#define RENDER_DISTANCE 6
#define MAX_THREADS 2
#define CHUNK_TO_WORLD(b, c) (((c) * Chunk::CHUNK_SIZE) + (b))

class WorldManager;

enum class BiomeType : uint8_t { kOCEAN, kPLAINS, kMOUNTAINS, kNONE };
enum class NoiseType : std::uint8_t {
    kTEMPERATURE,
    kHUMIDITY,
    kCONTINENT,
    kEROSION,
    kWEIRDNESS,
    kDEPTH
};
class Chunk;
using chunkMap = std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>>;

using ChunkCoord = glm::ivec3;

class Chunk {
  public:
    static constexpr int CHUNK_SIZE = 32;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

    Chunk(int x, int y, int z, WorldManager& worldManager);
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
    [[nodiscard]] bool isBlockSolid(int x, int y, int z, const BlockRegistry& registry) const;
    [[nodiscard]] static bool isInBounds(int x, int y, int z);
    [[nodiscard]] static int getIndex(int x, int y, int z);
    // [[nodiscard]] uint8_t getBiomeDataAt(int x, int z) const {
    //     int width = static_cast<int>(std::sqrt(_biomeData.size()));
    //     if (x < 0 || x >= width || z < 0 || z >= width) {
    //         return static_cast<uint8_t>(BiomeType::kNONE);
    //     }
    //     return _biomeData[(z * width + x)];
    // }
    // Chunk state
    [[nodiscard]] bool isEmpty() const { return _isEmpty; }
    void setEmpty(bool empty) { _isEmpty = empty; }

    // TODO - Cleanup this crap
    glm::ivec3 getPosition() const { return position; }
    [[nodiscard]] std::string getBiome(glm::ivec3 pos) const {
        if (_biomeData.size() < 1) {
            return "N/A";
        }
        switch (static_cast<BiomeType>(_biomeData[((pos[0] * CHUNK_SIZE) + pos[2])])) {
        case BiomeType::kPLAINS:
            return "Plains";
        case BiomeType::kMOUNTAINS:
            return "Mountains";
        case BiomeType::kOCEAN:
            return "Ocean";
        default:
            return "Unknown";
        }
    }
    [[nodiscard]] float getNoise(int x, int z, NoiseType type) const {
        int bx = CHUNK_TO_WORLD(x, position[0]);
        int bz = CHUNK_TO_WORLD(z, position[2]);
        switch (type) {
        case NoiseType::kTEMPERATURE:
            return perlinNoise(bx, bz, noise_config::kTEMPERATURE, kSEED);
        case NoiseType::kHUMIDITY:
            return perlinNoise(bx, bz, noise_config::kHUMIDITY, kSEED);
        case NoiseType::kCONTINENT:
            return perlinNoise(bx, bz, noise_config::kCONTINENT, kSEED);
        case NoiseType::kEROSION:
            return perlinNoise(bx, bz, noise_config::kEROSION, kSEED);
        case NoiseType::kWEIRDNESS:
            return perlinNoise(bx, bz, noise_config::kWEIRDNESS, kSEED);
        case NoiseType::kDEPTH:
            return perlinNoise(bx, bz, noise_config::kDEPTH, kSEED);
        default:
            return 0.0F;
        }
    }
    [[nodiscard]] BiomeParams getNoiseParams(int x, int z) const {
        int bx = CHUNK_TO_WORLD(x, position[0]);
        int bz = CHUNK_TO_WORLD(z, position[2]);
        BiomeParams biomeParams{};
        biomeParams.kTEMPERATURE = perlinNoise(bx, bz, noise_config::kTEMPERATURE, kSEED);
        biomeParams.humidity = perlinNoise(bx, bz, noise_config::kHUMIDITY, kSEED);
        biomeParams.continent = perlinNoise(bx, bz, noise_config::kCONTINENT, kSEED);
        biomeParams.erosion = perlinNoise(bx, bz, noise_config::kEROSION, kSEED);
        biomeParams.weirdness = perlinNoise(bx, bz, noise_config::kWEIRDNESS, kSEED);
        biomeParams.depth = perlinNoise(bx, bz, noise_config::kDEPTH, kSEED);
        return biomeParams;
    }
    BiomeType getBiomeDataAt(int x, int z) const {
        int width = static_cast<int>(std::sqrt(_biomeData.size()));
        if (x < 0 || x >= width || z < 0 || z >= width) {
            return BiomeType::kNONE;
        }
        return _biomeData[((x * width) + z)];
    }

  private:
    glm::ivec3 position{};
    std::array<uint8_t, VOLUME> _blocks{};
    bool _isEmpty = true;
    int8_t _biome = static_cast<int8_t>(BiomeType::kPLAINS);
    std::vector<BiomeType> _biomeData;
    void generateChunk();
    bool loadChunk();
    int64_t kSEED;
    // void saveChunk();
};

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "../Util/perlinNoise.hpp"
#include "common/World/ChunkMesh.hpp"
#include "glm/fwd.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#define RENDER_DISTANCE 6
#define MAX_THREADS 2

enum class BiomeType : uint8_t { PLAINS, MOUNTAINS, OCEAN };

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
    void setBlock(int x, int y, int z, uint8_t blockId);

    // Helper functions
    [[nodiscard]] bool isBlockSolid(int x, int y, int z) const;
    [[nodiscard]] bool isInBounds(int x, int y, int z) const;
    [[nodiscard]] int getIndex(int x, int y, int z) const;

    // Chunk state
    [[nodiscard]] bool isEmpty() const { return _isEmpty; }
    void setEmpty(bool empty) { _isEmpty = empty; }
    glm::ivec3 getPosition() const { return position; }

  private:
    glm::ivec3 position;
    std::array<uint8_t, VOLUME> _blocks;
    bool _isEmpty = true;

    void generateChunk();
    bool loadChunk();
    // void saveChunk();
};

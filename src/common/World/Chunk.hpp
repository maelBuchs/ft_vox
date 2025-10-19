#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>

#include "../Util/perlinNoise.hpp"
#include "glm/fwd.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#define RENDER_DISTANCE 64

class Chunk;
using chunkMap = std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>>;

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
    std::tuple<int, int, int> getPosition() const { return position; }

  private:
    std::tuple<int, int, int> position;
    std::array<uint8_t, VOLUME> _blocks;
    bool _isEmpty = true;
};

class ChunkInstanciator {
  public:
    ChunkInstanciator() = default;
    ~ChunkInstanciator() = default;
    ChunkInstanciator(const ChunkInstanciator&) = delete;
    ChunkInstanciator& operator=(const ChunkInstanciator&) = delete;
    ChunkInstanciator(ChunkInstanciator&&) = default;
    ChunkInstanciator& operator=(ChunkInstanciator&&) = default;
    // hecks which chunks need to be loaded/unloaded based on player position

    void updateChunksAroundPlayer(float playerX, float playerY, float playerZ, float viewDistance);
    [[nodiscard]] const chunkMap& getLoadedChunks() const { return _loadedChunks; }

  private:
    void loadChunkAt(int x, int y, int z);
    void unloadChunkAt(int x, int y, int z);
    // some data structures to hold loaded chunks
    chunkMap _loadedChunks;
};

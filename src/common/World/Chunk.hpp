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

class ChunkInstanciator {
  public:
    ChunkInstanciator() {
        for (int i = 0; i < MAX_THREADS - 1; ++i) {
            _workers.emplace_back(&ChunkInstanciator::startWorker, this);
        }
    }
    ~ChunkInstanciator() = default;
    ChunkInstanciator(const ChunkInstanciator&) = delete;
    ChunkInstanciator& operator=(const ChunkInstanciator&) = delete;
    ChunkInstanciator(ChunkInstanciator&&) = delete;
    ChunkInstanciator& operator=(ChunkInstanciator&&) = delete;
    std::vector<Chunk>& getLoadedChunks() {
        std::lock_guard<std::mutex> lock(_queueMutex);
        // return a copy to avoid data races
        for (auto& chunk : _readyChunks) {
            _loadedChunks.push_back(std::move(chunk));
        }
        _readyChunks.clear();
        return _loadedChunks;
    }
    // hecks which chunks need to be loaded/unloaded based on player position

    /*DONE */ void startWorker();
    // void startMeshWorker();
    void stopWorkers() {
        running = false;
        for (auto& worker : _workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    void update();
    void updateChunksAroundPlayer(glm::ivec3 playerChunkPos);
    bool isLoadedOrRequested(int x, int y, int z);
    void unloadChunkIfNeeded(glm::ivec3 playerChunkPos);

  private:
    std::vector<ChunkCoord> _chunksToGenerate;
    std::vector<Chunk> _readyChunks;
    std::vector<Chunk> _loadedChunks;
    std::unordered_map<glm::ivec3, bool> _loadedChunkMap;
    std::mutex _queueMutex;
    bool running = true;
    std::vector<std::thread> _workers;
};

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tracy/Tracy.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "common/Protocol/Protocol.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "common/Util/Util.hpp"
#include "common/World/Chunk.hpp"

#define SEED 42L

#define CHUNK_TO_WORLD(b, c) (((c) * Chunk::CHUNK_SIZE) + (b))

/**
 * WorldManager runs on dedicated generation worker threads.
 * Responsibilities:
 * - Receives ChunkRequest messages from the client
 * - Generates/loads chunk data (procedural or from disk) on multiple worker threads
 * - Pushes GenerationTask to meshing threads
 * - Manages chunk lifetime and caching
 * - Tracks in-progress chunks to avoid duplicate work
 */
class WorldManager {
  public:
    WorldManager(ThreadSafeQueue<ChunkRequest>& requestQueue,
                 ThreadSafeQueue<GenerationTask>& meshingQueue,
                 ThreadSafeQueue<MeshingComplete>& completionQueue);
    ~WorldManager();

    WorldManager(const WorldManager&) = delete;
    WorldManager& operator=(const WorldManager&) = delete;
    WorldManager(WorldManager&&) = delete;
    WorldManager& operator=(WorldManager&&) = delete;

    /**
     * Start the generation worker threads. Non-blocking.
     */
    void start();

    /**
     * Stop all worker threads. Blocks until all threads exit.
     */
    void stop();

    /**
     * Get current queue sizes for debugging/profiling.
     */
    struct QueueStats {
        size_t requestQueueSize;
        size_t meshingQueueSize;
        size_t loadedChunksCount;
        size_t generatingChunksCount;
        size_t meshingChunksCount;
        size_t chunksToUnloadCount;
    };
    QueueStats getQueueStats() const;

    /**
     * Mark a chunk for unloading. Thread-safe.
     * The chunk will be removed from memory during the next cleanup pass.
     */
    void markChunkForUnload(const glm::ivec3& pos);

    /**
     * Unmark a chunk for unloading (keep it loaded). Thread-safe.
     * Use this when a chunk that was previously marked comes back within range.
     */
    void unmarkChunkForUnload(const glm::ivec3& pos);

    /**
     * Unload all chunks marked for unloading. Returns the list of unloaded chunk positions.
     * Thread-safe. Call this from the main thread when ready to rebuild mesh pool.
     */
    std::vector<glm::ivec3> unloadMarkedChunks();

    /**
     * Get a copy of all currently loaded chunk positions. Thread-safe.
     * Useful for determining which chunks are far from camera and should be unloaded.
     */
    std::vector<glm::ivec3> getLoadedChunkPositions() const;

    /**
     * Check if a chunk is currently loaded in memory. Thread-safe.
     * @param pos Chunk position to check
     * @return true if chunk is loaded, false otherwise
     */
    bool isChunkLoaded(const glm::ivec3& pos) const;

    /**
     * Request re-meshing for all loaded chunks (except those in exclusion list).
     * Thread-safe. Use this after mesh pool rebuild to regenerate meshes.
     * @param excludeChunks Chunks to exclude from re-meshing (e.g., unloaded chunks)
     */
    void requestRemeshForAllChunks(const std::vector<glm::ivec3>& excludeChunks);

    Chunk* getCurrentChunk(const glm::vec3& position) {
        glm::ivec3 chunkPos = {static_cast<int>(std::floor(position[0] / 32)),
                               static_cast<int>(std::floor(position[1] / 32)),
                               static_cast<int>(std::floor(position[2] / 32))};

        std::lock_guard<std::mutex> lock(_chunkMutex);
        auto it = _loadedChunks.find(chunkPos);
        if (it != _loadedChunks.end() && it->second) {
            return it->second.get();
        }
        return nullptr; // Return nullptr if not found
    }

    tk::spline& getHeightSpline() { return _heightSpline; }

  private:
    /**
     * Generation worker thread main loop.
     * Processes chunk requests and generates/loads chunks.
     */
    void generationWorkerLoop();

    /**
     * Completion processing thread loop.
     * Handles meshing completion notifications and processes dirty chunks.
     */
    void completionProcessorLoop();

    /**
     * Generate a chunk at the given position.
     * Uses Perlin noise for terrain generation.
     */
    std::shared_ptr<Chunk> generateChunk(const glm::ivec3& pos);

    /**
     * Get a chunk from cache or return nullptr if not loaded.
     */
    std::shared_ptr<Chunk> getChunkFromCache(const glm::ivec3& pos);

    /**
     * Prepare and enqueue a meshing task for a chunk using up-to-date neighbors.
     * Returns true if task was enqueued, false if already in progress.
     */
    bool enqueueMeshingTask(const glm::ivec3& pos);

    /**
     * Internal version of enqueueMeshingTask that assumes _chunkMutex is already locked.
     * Used to avoid deadlock when called from functions that already hold the lock.
     */
    bool enqueueMeshingTaskInternal(const glm::ivec3& pos);

    /**
     * Mark neighbors as dirty for re-meshing.
     */
    void markNeighborsDirty(const glm::ivec3& pos);

    // Communication queues (owned by main_client.cpp, passed by reference)
    ThreadSafeQueue<ChunkRequest>& _requestQueue;
    ThreadSafeQueue<GenerationTask>& _meshingQueue;
    ThreadSafeQueue<MeshingComplete>& _completionQueue;

    // Thread control
    std::atomic<bool> _running{false};
    std::vector<std::unique_ptr<std::thread>> _generationWorkers;
    std::unique_ptr<std::thread> _completionProcessor;
    static constexpr int kNUM_GENERATION_WORKERS = 4; // Parallel generation threads

    // Chunk storage and tracking (protected by mutex)
    mutable std::mutex _chunkMutex;
    std::unordered_map<glm::ivec3, std::shared_ptr<Chunk>> _loadedChunks;
    std::unordered_set<glm::ivec3> _generatingChunks; // Chunks currently being generated
    std::unordered_set<glm::ivec3> _meshingChunks;    // Chunks currently being meshed
    std::unordered_set<glm::ivec3> _dirtyChunks;      // Chunks that need re-meshing
    std::unordered_set<glm::ivec3> _chunksToUnload;   // Chunks marked for unloading

    // Performance instrumentation
    std::atomic<uint64_t> _totalChunksGenerated{0};
    std::atomic<uint64_t> _totalMeshingTasksEnqueued{0};

    // World generation tools
    tk::spline _heightSpline;
};

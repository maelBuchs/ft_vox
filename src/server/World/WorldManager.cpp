#include "WorldManager.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

#include <tracy/Tracy.hpp>

#include "common/Util/Util.hpp"

WorldManager::WorldManager(ThreadSafeQueue<ChunkRequest>& requestQueue,
                           ThreadSafeQueue<GenerationTask>& meshingQueue,
                           ThreadSafeQueue<MeshingComplete>& completionQueue)
    : _requestQueue(requestQueue), _meshingQueue(meshingQueue), _completionQueue(completionQueue) {
    std::vector<double> x;
    std::vector<double> y;
    x = {-1.0, -0.6, -0.15, -0.05, 0.0, 0.05, 0.3, 0.6, 0.8, 1.0, 1.2};

    // Y : Hauteur (0-255)
    // 63 = Niveau de l'eau
    y = {20, 45, 58, 62, 63, 68, 80, 110, 170, 240, 255};
    _heightSpline.set_points(x, y, tk::spline::cspline);
}

WorldManager::~WorldManager() {
    stop();
}

void WorldManager::start() {
    if (_running.load()) {
        std::cerr << "[WorldManager] Already running!\n";
        return;
    }

    _running.store(true);
    _generationWorkers.reserve(kNUM_GENERATION_WORKERS);

    // Start generation workers
    for (int i = 0; i < kNUM_GENERATION_WORKERS; ++i) {
        _generationWorkers.push_back(
            std::make_unique<std::thread>(&WorldManager::generationWorkerLoop, this));
    }

    _completionProcessor =
        std::make_unique<std::thread>(&WorldManager::completionProcessorLoop, this);

    std::cout << "[WorldManager] Started " << kNUM_GENERATION_WORKERS
              << " generation workers + 1 completion processor\n";
}

void WorldManager::stop() {
    if (!_running.load()) {
        return;
    }

    std::cout << "[WorldManager] Stopping all threads...\n";
    _running.store(false);
    _requestQueue.shutdown();    // Wake up generation workers
    _completionQueue.shutdown(); // Wake up completion processor

    // Join generation workers
    for (auto& worker : _generationWorkers) {
        if (worker && worker->joinable()) {
            worker->join();
        }
    }
    _generationWorkers.clear();

    if (_completionProcessor && _completionProcessor->joinable()) {
        _completionProcessor->join();
    }

    {
        std::unique_lock<std::shared_mutex> mapLock(_chunkMapMutex);
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _loadedChunks.clear();
        _generatingChunks.clear();
        _meshingChunks.clear();
        _dirtyChunks.clear();
        _chunkLodLevels.clear();
    }
    std::cout << "[WorldManager] All threads stopped\n";
    std::cout << "[WorldManager] Stats - Chunks generated: " << _totalChunksGenerated.load()
              << ", Meshing tasks enqueued: " << _totalMeshingTasksEnqueued.load() << "\n";
}

void WorldManager::generationWorkerLoop() {
    ZoneScoped;
    ChunkRequest request{};

    while (_running.load()) {
        // Block until a chunk request arrives
        if (!_requestQueue.wait_and_pop(request)) {
            // Queue shutdown signal received
            break;
        }

        TracyMessageL("Processing chunk request");
        const glm::ivec3& pos = request.chunkPosition;

        bool shouldGenerate = false;

        // Check if chunk is already loaded or being generated
        {
            std::unique_lock<std::shared_mutex> mapLock(_chunkMapMutex);
            std::lock_guard<std::mutex> stateLock(_stateMutex);

            if (_loadedChunks.contains(pos)) {
                _chunkLodLevels[pos] = request.lodLevel;
                enqueueMeshingTaskInternal(pos, request.lodLevel);
                continue;
            }

            if (_generatingChunks.contains(pos)) {
                continue;
            }

            _generatingChunks.insert(pos);
            _chunkLodLevels[pos] = request.lodLevel;
            shouldGenerate = true;
        }
        if (!shouldGenerate) {
            continue;
        }

        // Generate the chunk (this is the expensive part - do it outside the lock)
        std::shared_ptr<Chunk> mainChunk = generateChunk(pos);

        {
            std::unique_lock<std::shared_mutex> mapLock(_chunkMapMutex);
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            _loadedChunks[pos] = mainChunk;
            _generatingChunks.erase(pos);
            _totalChunksGenerated.fetch_add(1);
        }

        // Mesh the newly generated chunk
        if (enqueueMeshingTask(pos, request.lodLevel)) {
            // Mark neighbors as dirty for re-meshing
            markNeighborsDirty(pos);
        }
    }
}

std::shared_ptr<Chunk> WorldManager::getChunkFromCache(const glm::ivec3& pos) {
    // Caller must hold necessary locks
    auto it = _loadedChunks.find(pos);
    if (it != _loadedChunks.end()) {
        return it->second;
    }
    return nullptr;
}

void WorldManager::markNeighborsDirty(const glm::ivec3& pos) {
    static constexpr glm::ivec3 kNEIGHBOR_OFFSETS[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);

    for (const glm::ivec3& offset : kNEIGHBOR_OFFSETS) {
        const glm::ivec3 kNEIGHBOR_POS = pos + offset;

        // Only mark dirty if chunk is loaded
        if (_loadedChunks.contains(kNEIGHBOR_POS)) {
            _dirtyChunks.insert(kNEIGHBOR_POS);

            // Count how many neighbors exist (loaded) vs how many are possible
            int loadedNeighbors = 0;
            int possibleNeighbors = 0;
            for (const glm::ivec3& checkOffset : kNEIGHBOR_OFFSETS) {
                const glm::ivec3 kCHECK_POS = kNEIGHBOR_POS + checkOffset;

                if (kCHECK_POS[1] >= 0) {
                    possibleNeighbors++;
                    if (_loadedChunks.contains(kCHECK_POS)) {
                        loadedNeighbors++;
                    }
                }
            }

            // Re-mesh ONLY if chunk has ALL POSSIBLE neighbors loaded
            if (loadedNeighbors == possibleNeighbors && !_meshingChunks.contains(kNEIGHBOR_POS)) {
                _dirtyChunks.erase(kNEIGHBOR_POS);
                _meshingChunks.insert(kNEIGHBOR_POS);

                const uint32_t lodLevel =
                    _chunkLodLevels.contains(kNEIGHBOR_POS) ? _chunkLodLevels[kNEIGHBOR_POS] : 0;
                GenerationTask task(kNEIGHBOR_POS, _loadedChunks[kNEIGHBOR_POS], lodLevel);
                task.neighborNorth = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0], kNEIGHBOR_POS[1], kNEIGHBOR_POS[2] + 1));
                task.neighborSouth = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0], kNEIGHBOR_POS[1], kNEIGHBOR_POS[2] - 1));
                task.neighborEast = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0] + 1, kNEIGHBOR_POS[1], kNEIGHBOR_POS[2]));
                task.neighborWest = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0] - 1, kNEIGHBOR_POS[1], kNEIGHBOR_POS[2]));
                task.neighborTop = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0], kNEIGHBOR_POS[1] + 1, kNEIGHBOR_POS[2]));
                task.neighborBottom = getChunkFromCache(
                    glm::ivec3(kNEIGHBOR_POS[0], kNEIGHBOR_POS[1] - 1, kNEIGHBOR_POS[2]));

                _meshingQueue.push(std::move(task));
                _totalMeshingTasksEnqueued.fetch_add(1);
            }
        }
    }
}

WorldManager::QueueStats WorldManager::getQueueStats() const {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    return QueueStats{_requestQueue.size(),     _meshingQueue.size(),  _loadedChunks.size(),
                      _generatingChunks.size(), _meshingChunks.size(), _chunksToUnload.size()};
}

void WorldManager::completionProcessorLoop() {
    ZoneScoped;
    MeshingComplete completion{};

    while (_running.load()) {
        // Block until a completion notification arrives
        if (!_completionQueue.wait_and_pop(completion)) {
            // Queue shutdown signal received
            break;
        }

        const glm::ivec3& pos = completion.chunkPosition;

        std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
        std::lock_guard<std::mutex> stateLock(_stateMutex);

        // Remove from meshing set
        _meshingChunks.erase(pos);

        // Check if chunk is dirty (needs re-meshing)
        if (_dirtyChunks.contains(pos)) {
            static constexpr glm::ivec3 kNEIGHBOR_OFFSETS[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            int loadedNeighbors = 0;
            int possibleNeighbors = 0;
            for (const glm::ivec3& offset : kNEIGHBOR_OFFSETS) {
                const glm::ivec3 kCHECK_POS = pos + offset;

                if (kCHECK_POS[1] >= 0) {
                    possibleNeighbors++;
                    if (_loadedChunks.contains(kCHECK_POS)) {
                        loadedNeighbors++;
                    }
                }
            }

            if (loadedNeighbors == possibleNeighbors) {
                _dirtyChunks.erase(pos);

                if (_loadedChunks.contains(pos) && !_meshingChunks.contains(pos)) {

                    _meshingChunks.insert(pos);

                    const uint32_t lodLevel = _chunkLodLevels.contains(pos) ? _chunkLodLevels[pos] : 0;
                    GenerationTask task(pos, _loadedChunks[pos], lodLevel);
                    task.neighborNorth = getChunkFromCache(glm::ivec3(pos[0], pos[1], pos[2] + 1));
                    task.neighborSouth = getChunkFromCache(glm::ivec3(pos[0], pos[1], pos[2] - 1));
                    task.neighborEast = getChunkFromCache(glm::ivec3(pos[0] + 1, pos[1], pos[2]));
                    task.neighborWest = getChunkFromCache(glm::ivec3(pos[0] - 1, pos[1], pos[2]));
                    task.neighborTop = getChunkFromCache(glm::ivec3(pos[0], pos[1] + 1, pos[2]));
                    task.neighborBottom = getChunkFromCache(glm::ivec3(pos[0], pos[1] - 1, pos[2]));

                    _meshingQueue.push(std::move(task));
                    _totalMeshingTasksEnqueued.fetch_add(1);
                }
            }
        }
    }
}

bool WorldManager::enqueueMeshingTask(const glm::ivec3& pos) {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    const uint32_t lodLevel = _chunkLodLevels.contains(pos) ? _chunkLodLevels[pos] : 0;
    return enqueueMeshingTaskInternal(pos, lodLevel);
}

bool WorldManager::enqueueMeshingTask(const glm::ivec3& pos, uint32_t lodLevel) {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    _chunkLodLevels[pos] = lodLevel;
    return enqueueMeshingTaskInternal(pos, lodLevel);
}

bool WorldManager::enqueueMeshingTaskInternal(const glm::ivec3& pos, uint32_t lodLevel) {
    // NOTE: Caller must hold _chunkMapMutex (shared) and _stateMutex before calling

    auto chunkIt = _loadedChunks.find(pos);
    if (chunkIt == _loadedChunks.end()) {
        return false;
    }

    // Skip if already being meshed (deduplication)
    if (_meshingChunks.contains(pos)) {
        // Mark as dirty so it gets re-meshed after current mesh completes
        _dirtyChunks.insert(pos);
        return false;
    }

    // Mark as being meshed
    _meshingChunks.insert(pos);

    GenerationTask task(pos, chunkIt->second, lodLevel);
    task.neighborNorth = getChunkFromCache(glm::ivec3(pos[0], pos[1], pos[2] + 1));
    task.neighborSouth = getChunkFromCache(glm::ivec3(pos[0], pos[1], pos[2] - 1));
    task.neighborEast = getChunkFromCache(glm::ivec3(pos[0] + 1, pos[1], pos[2]));
    task.neighborWest = getChunkFromCache(glm::ivec3(pos[0] - 1, pos[1], pos[2]));
    task.neighborTop = getChunkFromCache(glm::ivec3(pos[0], pos[1] + 1, pos[2]));
    task.neighborBottom = getChunkFromCache(glm::ivec3(pos[0], pos[1] - 1, pos[2]));

    _meshingQueue.push(std::move(task));
    _totalMeshingTasksEnqueued.fetch_add(1);
    return true;
}

void WorldManager::markChunkForUnload(const glm::ivec3& pos) {
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    _chunksToUnload.insert(pos);
}

void WorldManager::unmarkChunkForUnload(const glm::ivec3& pos) {
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    _chunksToUnload.erase(pos);
}

std::vector<glm::ivec3> WorldManager::unloadMarkedChunks() {
    std::unique_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    std::vector<glm::ivec3> unloadedChunks;
    unloadedChunks.reserve(_chunksToUnload.size());

    for (const glm::ivec3& pos : _chunksToUnload) {
        auto it = _loadedChunks.find(pos);
        if (it != _loadedChunks.end()) {
            _loadedChunks.erase(it);
            unloadedChunks.push_back(pos);
        }

        _generatingChunks.erase(pos);
        _meshingChunks.erase(pos);
        _dirtyChunks.erase(pos);
        _chunkLodLevels.erase(pos);
    }

    _chunksToUnload.clear();

    if (!unloadedChunks.empty()) {
        std::cout << "[WorldManager] Unloaded " << unloadedChunks.size() << " chunks from RAM\n";
    }

    return unloadedChunks;
}

std::vector<glm::ivec3> WorldManager::getLoadedChunkPositions() const {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::vector<glm::ivec3> positions;
    positions.reserve(_loadedChunks.size());

    for (const auto& [pos, chunk] : _loadedChunks) {
        positions.push_back(pos);
    }

    return positions;
}

bool WorldManager::isChunkLoaded(const glm::ivec3& pos) const {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    return _loadedChunks.contains(pos);
}

void WorldManager::requestRemeshForAllChunks(const std::vector<glm::ivec3>& excludeChunks) {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);

    // Convert exclude list to set for fast lookup
    std::unordered_set<glm::ivec3> excludeSet(excludeChunks.begin(), excludeChunks.end());

    int remeshedCount = 0;
    for (const auto& [pos, chunk] : _loadedChunks) {
        if (excludeSet.contains(pos)) {
            continue;
        }

        _dirtyChunks.insert(pos);

        const uint32_t lodLevel = _chunkLodLevels.contains(pos) ? _chunkLodLevels[pos] : 0;
        enqueueMeshingTaskInternal(pos, lodLevel);
        remeshedCount++;
    }

    std::cout << "[WorldManager] Requested re-meshing for " << remeshedCount << " loaded chunks\n";
}

void WorldManager::remeshChunkAtPosition(const glm::ivec3& pos) {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    std::lock_guard<std::mutex> stateLock(_stateMutex);
    _dirtyChunks.insert(pos);
    const uint32_t lodLevel = _chunkLodLevels.contains(pos) ? _chunkLodLevels[pos] : 0;
    enqueueMeshingTaskInternal(pos, lodLevel);
}

void WorldManager::updatedBlockAt(glm::ivec3 worldPos) {
    glm::ivec3 chunkPos =
        glm::ivec3(worldToChunk(worldPos.x), worldToChunk(worldPos.y), worldToChunk(worldPos.z));
    remeshChunkAtPosition(chunkPos);
    if (worldToBlock(worldPos.x) == Chunk::CHUNK_SIZE - 1) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(1, 0, 0));
    } else if (worldToBlock(worldPos.x) == 0) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(-1, 0, 0));
    }
    if (worldToBlock(worldPos.y) == Chunk::CHUNK_SIZE - 1) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(0, 1, 0));
    } else if (worldToBlock(worldPos.y) == 0) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(0, -1, 0));
    }
    if (worldToBlock(worldPos.z) == Chunk::CHUNK_SIZE - 1) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(0, 0, 1));
    } else if (worldToBlock(worldPos.z) == 0) {
        remeshChunkAtPosition(chunkPos + glm::ivec3(0, 0, -1));
    }
}

std::optional<glm::vec3> WorldManager::getTargetBlock(const Camera& camera) {
    glm::vec3 start = camera.getPosition();
    glm::vec3 direction = glm::normalize(camera.getFront());

    int voxelX = std::floor(start[0]);
    int voxelY = std::floor(start[1]);
    int voxelZ = std::floor(start[2]);

    int stepX = (direction[0] > 0) ? 1 : -1;
    int stepY = (direction[1] > 0) ? 1 : -1;
    int stepZ = (direction[2] > 0) ? 1 : -1;

    // Utilisation de 1e30 pour éviter la division par zéro si direction est 0
    float tDeltaX = (direction[0] == 0) ? 1e30F : std::abs(1.0F / direction[0]);
    float tDeltaY = (direction[1] == 0) ? 1e30F : std::abs(1.0F / direction[1]);
    float tDeltaZ = (direction[2] == 0) ? 1e30F : std::abs(1.0F / direction[2]);

    float tMaxX = (direction[0] > 0 ? (std::floor(start[0]) + 1.0F - start[0])
                                    : (start[0] - std::floor(start[0]))) *
                  tDeltaX;
    float tMaxY = (direction[1] > 0 ? (std::floor(start[1]) + 1.0F - start[1])
                                    : (start[1] - std::floor(start[1]))) *
                  tDeltaY;
    float tMaxZ = (direction[2] > 0 ? (std::floor(start[2]) + 1.0F - start[2])
                                    : (start[2] - std::floor(start[2]))) *
                  tDeltaZ;

    while (true) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            if (tMaxX > REACH_DISTANCE) {
                break;
            }
            voxelX += stepX;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            if (tMaxY > REACH_DISTANCE) {
                break;
            }
            voxelY += stepY;
            tMaxY += tDeltaY;
        } else {
            if (tMaxZ > REACH_DISTANCE) {
                break;
            }
            voxelZ += stepZ;
            tMaxZ += tDeltaZ;
        }

        auto block = getBlockValue({voxelX, voxelY, voxelZ});
        if (block != 0 && block != 8) {
            return glm::vec3(voxelX, voxelY, voxelZ);
        }
    }

    return std::nullopt;
}

void WorldManager::setBlockValue(glm::vec3 position, uint8_t blockId) {
    glm::ivec3 chunkPosition = glm::ivec3(worldToChunk(static_cast<int>(position[0])),
                                          worldToChunk(static_cast<int>(position[1])),
                                          worldToChunk(static_cast<int>(position[2])));
    std::shared_ptr<Chunk> chunk = getChunkAtPosition(chunkPosition);
    if (chunk) {
        chunk->setBlock(worldToBlock(static_cast<int>(position[0])),
                        worldToBlock(static_cast<int>(position[1])),
                        worldToBlock(static_cast<int>(position[2])), blockId);
    }
}

uint8_t WorldManager::getBlockValue(glm::vec3 position) {

    glm::ivec3 chunkPosition = glm::ivec3(worldToChunk(static_cast<int>(position[0])),
                                          worldToChunk(static_cast<int>(position[1])),
                                          worldToChunk(static_cast<int>(position[2])));
    std::shared_ptr<Chunk> chunk = getChunkAtPosition(chunkPosition);
    if (chunk) {
        return chunk->getBlock(worldToBlock(static_cast<int>(position[0])),
                               worldToBlock(static_cast<int>(position[1])),
                               worldToBlock(static_cast<int>(position[2])));
    }
    return 0;
}

std::shared_ptr<Chunk> WorldManager::getChunkAtPosition(glm::ivec3 target) {
    std::shared_lock<std::shared_mutex> mapLock(_chunkMapMutex);
    auto it = _loadedChunks.find(target);
    if (it != _loadedChunks.end() && it->second) {
        return (it->second);
    }
    return nullptr;
}

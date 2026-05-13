#pragma once

#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_oldnames.h>

#include <glm/vec3.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "common/Protocol/Protocol.hpp"
#include "common/Util/ThreadSafeQueue.hpp"

class Window;
class VulkanDevice;
class Renderer;
class BlockRegistry;
class WorldManager;
class MeshingThread;
class Camera;
class InputManager;
class App {
  public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    static constexpr int WIDTH = 800;
    static constexpr int HEIGHT = 600;
    static constexpr const char* WINDOW_TITLE = "Vulkan App";

    void run();

    [[nodiscard]] float getTimeOfDay() const { return _timeOfDay; }
    void setTimeOfDay(float time) { _timeOfDay = time; }

  private:
    /**
     * @brief Enqueue chunk generation requests for the surrounding chunks.
     *
     * @param centerChunk The center chunk position.
     */
    void enqueueChunkRequests(const glm::ivec3& centerChunk);
    void manageInputs(InputManager& inputManager, Window& window, SDL_Event event);

    /**
     * @brief Rebuild the chunk request offsets based on the given radius.
     *
     * @param radius The radius around the center chunk to rebuild offsets for.
     */
    void rebuildChunkOffsets(int radius);

    void updateRender(Camera& camera, InputManager& inputManager);

    void updateUI(Renderer& renderer, InputManager& inputManager, Camera& camera,
                  const glm::ivec3& currentCenter, const std::shared_ptr<Chunk> currentChunk);
    std::unique_ptr<BlockRegistry> _blockRegistry;
    std::unique_ptr<Window> _window;
    std::unique_ptr<VulkanDevice> _vulkanDevice;
    std::unique_ptr<Renderer> _renderer;

    // Time of day system (0.0 = midnight, 0.5 = noon, 1.0 = midnight)
    float _timeOfDay = 0.5F;  // Start at noon
    float _timeSpeed = 0.02F; // Speed of time progression (adjustable)

    // UI mode for ImGui interaction
    bool _uiMode = false;

    // === Async chunk loading system ===
    // Communication queues (owned by App, passed by reference to workers)
    ThreadSafeQueue<ChunkRequest> _chunkRequestQueue;
    ThreadSafeQueue<GenerationTask> _meshingTaskQueue;
    // PER-THREAD mesh queues to eliminate lock contention (each thread gets its own queue)
    std::vector<std::unique_ptr<ThreadSafeQueue<MeshData>>> _perThreadMeshQueues;
    ThreadSafeQueue<MeshingComplete> _meshingCompleteQueue;

    // Configurable chunk load radius (in chunks)
    int _loadRadius = 8;
    int _lastLoadRadius = -1;

    // Request throttling
    static constexpr int MAX_REQUESTS_PER_FRAME = 512; // Don't flood the queue
    int _currentShellRadius = 0;                       // For breadth-first chunk loading
    std::unordered_map<glm::ivec3, uint32_t> _requestedChunks; // Track requested chunks and target LOD

    // Worker threads
    std::unique_ptr<WorldManager> _worldManager;
    std::vector<std::unique_ptr<MeshingThread>> _meshingThreads;
    static constexpr int NUM_MESHING_WORKERS = 4; // Parallel meshing workers

    glm::ivec3 _lastRequestedCenter{std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::min()};
    bool _needsRequestRefresh = true;
    std::vector<glm::ivec3> _chunkRequestOffsets;

    // === Chunk unloading system ===
    static constexpr int UNLOAD_CHECK_INTERVAL = 30;   // Check every 0.5 seconds at 60fps (was 120)
    static constexpr size_t REBUILD_THRESHOLD = 200;   // Minimum chunks to trigger rebuild
    static constexpr float REBUILD_PERCENTAGE = 0.10F; // OR 10% of loaded chunks
    static constexpr int VERTICAL_CHUNK_MIN = -400;    // Minimum vertical chunk coordinate
    static constexpr int VERTICAL_CHUNK_MAX = 400;     // Maximum vertical chunk coordinate
    int _framesSinceUnloadCheck = 0;
    float _unloadDistanceMultiplier = 2.5F; // Unload chunks 2.5x farther than load radius
};

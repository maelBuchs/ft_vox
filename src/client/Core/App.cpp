#include "App.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

#include "client/Game/Camera.hpp"
#include "client/Graphics/Core/VulkanDevice.hpp"
#include "client/Graphics/Renderer.hpp"
#include "common/World/BlockRegistry.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "InputManager.hpp"
#include "server/World/MeshingThread.hpp"
#include "server/World/WorldManager.hpp"
#include "Window.hpp"

App::App() {
    try {
        _blockRegistry = std::make_unique<BlockRegistry>();
        _window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
        _vulkanDevice = std::make_unique<VulkanDevice>(_window->getSDLWindow());

        // Create renderer, passing the finished mesh queue
        _renderer = std::make_unique<Renderer>(*_window, *_vulkanDevice, *_blockRegistry,
                                               _finishedMeshQueue);

        // Initialize worker threads for async chunk loading
        _worldManager = std::make_unique<WorldManager>(_chunkRequestQueue, _meshingTaskQueue,
                                                       _meshingCompleteQueue);

        // Create texture resolver lambda for meshing threads
        auto textureResolver = [this](const std::string& path) -> uint32_t {
            return _renderer->getTextureId(path);
        };

        // Create multiple meshing workers for parallel processing
        _meshingThreads.reserve(NUM_MESHING_WORKERS);
        for (int i = 0; i < NUM_MESHING_WORKERS; ++i) {
            _meshingThreads.push_back(std::make_unique<MeshingThread>(
                _meshingTaskQueue, _finishedMeshQueue, _meshingCompleteQueue, *_blockRegistry,
                textureResolver));
        }

        // Start worker threads
        _worldManager->start();
        for (auto& worker : _meshingThreads) {
            worker->start();
        }

        std::cout << "[App] Async chunk loading system initialized with " << NUM_MESHING_WORKERS
                  << " meshing workers\n";
    } catch (const std::exception& e) {
        std::cerr << "Failed to create window: " << e.what() << "\n";
        throw;
    }
}

App::~App() {
    // Stop worker threads before destroying other resources
    for (auto& worker : _meshingThreads) {
        if (worker) {
            worker->stop();
        }
    }
    if (_worldManager) {
        _worldManager->stop();
    }
}

void App::run() {
    if (!_window) {
        std::cerr << "Window not initialized!\n";
        return;
    }

    InputManager inputManager;
    SDL_Event event;

    SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), true);
    std::cout << "[APP] Camera controls: WASD to move, Mouse to look, ESC to quit\n";
    std::cout << "[APP] Press F1 to toggle wireframe mode\n";

    // Delta time tracking
    uint64_t lastTime = SDL_GetPerformanceCounter();
    const uint64_t perfFrequency = SDL_GetPerformanceFrequency();

    while (!inputManager.shouldQuit()) {
        // Calculate delta time
        uint64_t currentTime = SDL_GetPerformanceCounter();
        float deltaTime =
            static_cast<float>(currentTime - lastTime) / static_cast<float>(perfFrequency);
        lastTime = currentTime;

        inputManager.newFrame();

        // Handle Escape to toggle UI mode
        if (inputManager.isEscapePressed()) {
            _uiMode = !_uiMode;
            SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), !_uiMode);
        }

        while (SDL_PollEvent(&event)) {
            // Handle window resize
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                _renderer->resizeSwapchain();
            }

            inputManager.processEvent(event);
            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        // Toggle wireframe mode with F1
        if (inputManager.isWireframeToggled()) {
            _renderer->setWireframeMode(!_renderer->isWireframeMode());
        }

        // Update camera based on input (only if not in UI mode)
        Camera& camera = _renderer->getCamera();
        if (!_uiMode) {
            inputManager.updateCamera(camera, deltaTime);
        }

        // Request chunks around the camera (async)
        // Convert camera position to chunk coordinates
        const glm::vec3 cameraPos = camera.getPosition();
        const int chunkX = static_cast<int>(std::floor(cameraPos.x / 32.0F));
        const int chunkY = static_cast<int>(std::floor(cameraPos.y / 32.0F));
        const int chunkZ = static_cast<int>(std::floor(cameraPos.z / 32.0F));

        const glm::ivec3 currentCenter(chunkX, chunkY, chunkZ);
        if (_needsRequestRefresh || currentCenter != _lastRequestedCenter ||
            _loadRadius != _lastLoadRadius) {
            enqueueChunkRequests(currentCenter);
        }

        // === Chunk Unloading System ===
        // Periodically check for chunks that are too far from camera and mark them for unloading
        _framesSinceUnloadCheck++;
        if (_framesSinceUnloadCheck >= UNLOAD_CHECK_INTERVAL) {
            _framesSinceUnloadCheck = 0;

            // Use Chebyshev distance (same as loading) for consistent behavior
            const int unloadRadius =
                static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);

            // Get all loaded chunks from WorldManager
            std::vector<glm::ivec3> loadedChunks = _worldManager->getLoadedChunkPositions();

            // Update unload marks based on current camera position
            int markedCount = 0;
            int unmarkedCount = 0;
            for (const glm::ivec3& chunkPos : loadedChunks) {
                // Calculate offset from camera chunk position
                const glm::ivec3 offset = chunkPos - currentCenter;

                // Use Chebyshev distance (max of absolute values) - same as loading pattern
                const int chebyshevDistance =
                    std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

                if (chebyshevDistance > unloadRadius) {
                    // Chunk is far away - mark for unload
                    _worldManager->markChunkForUnload(chunkPos);
                    _requestedChunks.erase(chunkPos); // Remove from requested set
                    markedCount++;
                } else if (chebyshevDistance <= _loadRadius) {
                    // Chunk is within LOAD radius (not just unload radius) - unmark if previously
                    // marked Only unmark chunks that are close enough to be actively used
                    _worldManager->unmarkChunkForUnload(chunkPos);
                    unmarkedCount++;
                }
                // Chunks in the "buffer zone" (between loadRadius and unloadRadius) stay as they
                // are
            }

            // Get current unload queue size
            auto queueStats = _worldManager->getQueueStats();
            const size_t totalMarkedForUnload = queueStats.chunksToUnloadCount;

            // Trigger unload more aggressively - lower thresholds
            const size_t REBUILD_THRESHOLD = 200;   // Minimum chunks to trigger rebuild (was 200)
            const float REBUILD_PERCENTAGE = 0.10F; // OR 10% of loaded chunks (was 15%)

            if (totalMarkedForUnload >= REBUILD_THRESHOLD ||
                (totalMarkedForUnload > 0 && loadedChunks.size() > 0 &&
                 static_cast<float>(totalMarkedForUnload) /
                         static_cast<float>(loadedChunks.size()) >=
                     REBUILD_PERCENTAGE)) {

                std::cout << "[App] Unload check: camera at chunk (" << currentCenter.x << ", "
                          << currentCenter.y << ", " << currentCenter.z << ")\n";
                std::cout << "[App] Load radius: " << _loadRadius
                          << " chunks, Unload radius: " << unloadRadius << " chunks\n";
                std::cout << "[App] Marked " << totalMarkedForUnload
                          << " chunks total for unload (this check: " << markedCount
                          << "). Triggering rebuild...\n";

                std::vector<glm::ivec3> unloadedChunks = _worldManager->unloadMarkedChunks();
                if (!unloadedChunks.empty()) {
                    // Remove unloaded chunks from renderer tracking
                    // Note: This does NOT reset the mesh pool, so other chunks keep their
                    // allocations
                    _renderer->rebuildMeshPool(unloadedChunks);

                    // No need to re-mesh other chunks - they keep their existing mesh allocations
                }
            } else if (markedCount > 0) {
                std::cout << "[App] Marked " << markedCount << " chunks for unload"
                          << " (total: " << totalMarkedForUnload << ", waiting for "
                          << REBUILD_THRESHOLD << " threshold)\n";
            }
        }

        // Update time of day
        _timeOfDay += _timeSpeed * deltaTime;
        if (_timeOfDay > 1.0F) {
            _timeOfDay -= 1.0F; // Wrap around
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ImGui UI for FPS and wireframe toggle
        ImGui::Begin("Debug Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS: %.1f", _renderer->getFPS());
        ImGui::Text("Frame Time: %.3f ms", deltaTime * 1000.0F);
        ImGui::Separator();

        // Worker thread info
        ImGui::Text("Workers: %d meshing threads", NUM_MESHING_WORKERS);

        // Queue statistics
        auto queueStats = _worldManager->getQueueStats();
        ImGui::Separator();
        ImGui::Text("Queue Statistics:");
        ImGui::Text("  Request Queue: %zu", queueStats.requestQueueSize);
        ImGui::Text("  Meshing Queue: %zu", queueStats.meshingQueueSize);
        ImGui::Text("  Loaded Chunks (RAM): %zu", queueStats.loadedChunksCount);
        ImGui::Text("  Loaded Chunks (VRAM): %zu", _renderer->getLoadedChunkCount());
        ImGui::Text("  Generating: %zu", queueStats.generatingChunksCount);
        ImGui::Text("  Meshing: %zu", queueStats.meshingChunksCount);
        ImGui::Text("  Requested: %zu", _requestedChunks.size());
        ImGui::Text("  Marked for Unload: %zu", queueStats.chunksToUnloadCount);
        ImGui::Separator();

        bool wireframeMode = _renderer->isWireframeMode();
        if (ImGui::Checkbox("Wireframe Mode (F1)", &wireframeMode)) {
            _renderer->setWireframeMode(wireframeMode);
        }

        // Runtime control for how many chunks to load in each direction
        if (ImGui::SliderInt("Chunk Load Radius", &_loadRadius, 1, 64)) {
            _needsRequestRefresh = true;
        }

        // Chunk unloading controls
        ImGui::SliderFloat("Unload Distance Multiplier", &_unloadDistanceMultiplier, 1.5F, 4.0F);
        const int currentUnloadRadius =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        ImGui::Text("  Unload Radius: %d chunks (buffer: %d)", currentUnloadRadius,
                    currentUnloadRadius - _loadRadius);

        ImGui::Separator();
        const glm::vec3 camPos = camera.getPosition();
        ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
        ImGui::Text("Camera Chunk: (%d, %d, %d)", currentCenter.x, currentCenter.y,
                    currentCenter.z);

        ImGui::Separator();
        ImGui::Text("Sky System");
        if (ImGui::SliderFloat("Time of Day", &_timeOfDay, 0.0F, 1.0F)) {
            // Slider changed, time is now manually controlled
        }
        ImGui::SliderFloat("Time Speed", &_timeSpeed, 0.0F, 0.2F);

        // Display time as readable format
        int hours = static_cast<int>(_timeOfDay * 24.0F);
        int minutes = static_cast<int>((_timeOfDay * 24.0F - hours) * 60.0F);
        ImGui::Text("Current Time: %02d:%02d", hours, minutes);

        ImGui::Separator();
        if (ImGui::Button("Quit")) {
            inputManager.setShouldQuit(true);
        }

        ImGui::End();

        ImGui::Render();

        // Process finished mesh data from worker threads (non-blocking)
        // Pass current camera position and load radius to reject meshes that are too far
        // Use unload radius to be more lenient - accept meshes up to unload distance
        const int acceptDistance =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        _renderer->updateMeshes(currentCenter, acceptDistance);

        // Update FPS counter
        _renderer->updateFPS(deltaTime);

        _renderer->draw(_timeOfDay);
    }
}

void App::enqueueChunkRequests(const glm::ivec3& centerChunk) {
    // If center changed, clean up distant chunks from _requestedChunks instead of clearing all
    if (centerChunk != _lastRequestedCenter) {
        _currentShellRadius = 0;

        // Only remove chunks that are now far away (beyond unload distance)
        // This keeps tracking of what's actually loaded
        const int unloadRadius =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        std::unordered_set<glm::ivec3> chunksToRemove;

        for (const glm::ivec3& requestedPos : _requestedChunks) {
            const glm::ivec3 offset = requestedPos - centerChunk;
            const int chebyshevDistance =
                std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

            // Remove from requested set if beyond unload radius
            if (chebyshevDistance > unloadRadius) {
                chunksToRemove.insert(requestedPos);
            }
        }

        for (const glm::ivec3& pos : chunksToRemove) {
            _requestedChunks.erase(pos);
        }
    }

    if (_loadRadius != _lastLoadRadius || _chunkRequestOffsets.empty()) {
        rebuildChunkOffsets(_loadRadius);
        _lastLoadRadius = _loadRadius;
    }

    // Throttle requests: only enqueue MAX_REQUESTS_PER_FRAME per frame
    // Use breadth-first approach (shell-by-shell)
    int requestsThisFrame = 0;

    for (const glm::ivec3& offset : _chunkRequestOffsets) {
        // Calculate shell radius
        int shellRadius = std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

        // Skip chunks from earlier shells (already requested)
        if (shellRadius < _currentShellRadius) {
            continue;
        }

        // Move to next shell if we've exceeded frame budget
        if (shellRadius > _currentShellRadius) {
            if (requestsThisFrame >= MAX_REQUESTS_PER_FRAME) {
                break; // Continue next frame
            }
            _currentShellRadius = shellRadius;
        }

        const glm::ivec3 chunkPos = centerChunk + offset;

        // Check current distance to ensure chunk is actually within range
        const glm::ivec3 distCheck = chunkPos - centerChunk;
        const int currentDistance =
            std::max({std::abs(distCheck.x), std::abs(distCheck.y), std::abs(distCheck.z)});

        // Skip if beyond load radius (safety check)
        if (currentDistance > _loadRadius) {
            continue;
        }

        // Skip if already in requested set (already loaded or in queue)
        if (_requestedChunks.find(chunkPos) != _requestedChunks.end()) {
            continue;
        }

        // Enqueue the request
        if (chunkPos[1] >= 0) {
            // CRITICAL FIX: Only unmark from unload queue if the chunk is NOT already loaded
            // If it's already loaded, don't interfere with the unload system
            if (!_worldManager->isChunkLoaded(chunkPos)) {
                // Chunk is not loaded yet, safe to unmark (in case it was marked before)
                _worldManager->unmarkChunkForUnload(chunkPos);

                // Enqueue request for generation
                _chunkRequestQueue.push(ChunkRequest(chunkPos));
                requestsThisFrame++;
            }

            // Mark as requested regardless (tracks both loaded and in-queue chunks)
            _requestedChunks.insert(chunkPos);
        }

        if (requestsThisFrame >= MAX_REQUESTS_PER_FRAME) {
            break; // Continue next frame
        }
    }

    _lastRequestedCenter = centerChunk;
    _needsRequestRefresh = false;
}

void App::rebuildChunkOffsets(int radius) {
    _chunkRequestOffsets.clear();

    const int range = radius;
    const int verticalMin = -400;
    const int verticalMax = 400;

    _chunkRequestOffsets.reserve(static_cast<std::size_t>((2 * range + 1) * (2 * range + 1) *
                                                          (verticalMax - verticalMin + 1)));

    for (int dx = -range; dx <= range; ++dx) {
        for (int dy = verticalMin; dy <= verticalMax; ++dy) {
            for (int dz = -range; dz <= range; ++dz) {
                _chunkRequestOffsets.emplace_back(dx, dy, dz);
            }
        }
    }

    // Sort by shell radius (Chebyshev distance) for breadth-first loading
    // Chunks in the same shell are sorted by squared Euclidean distance
    auto shellRadius = [](const glm::ivec3& offset) {
        return std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});
    };

    auto squaredDistance = [](const glm::ivec3& offset) {
        return offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
    };

    std::sort(_chunkRequestOffsets.begin(), _chunkRequestOffsets.end(),
              [&shellRadius, &squaredDistance](const glm::ivec3& a, const glm::ivec3& b) {
                  const int shellA = shellRadius(a);
                  const int shellB = shellRadius(b);
                  if (shellA != shellB) {
                      return shellA < shellB; // Closer shells first
                  }
                  // Within same shell, sort by Euclidean distance
                  return squaredDistance(a) < squaredDistance(b);
              });
}

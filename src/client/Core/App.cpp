#include "App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <tracy/Tracy.hpp>

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

#ifndef CHUNK_TO_WORLD

#define CHUNK_TO_WORLD(b, c) (((c) * 32) + (b))
#endif
#define WORLD_TO_CHUNK(b) ((((b) % 32) + 32) % 32)

App::App() {
    try {
        _blockRegistry = std::make_unique<BlockRegistry>();
        _window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
        _vulkanDevice = std::make_unique<VulkanDevice>(_window->getSDLWindow());

        _perThreadMeshQueues.reserve(NUM_MESHING_WORKERS);
        for (int i = 0; i < NUM_MESHING_WORKERS; ++i) {
            _perThreadMeshQueues.push_back(std::make_unique<ThreadSafeQueue<MeshData>>());
        }

        _renderer = std::make_unique<Renderer>(*_window, *_vulkanDevice, *_blockRegistry,
                                               _perThreadMeshQueues);

        _worldManager = std::make_unique<WorldManager>(_chunkRequestQueue, _meshingTaskQueue,
                                                       _meshingCompleteQueue);

        // Create texture resolver lambda for meshing threads
        auto textureResolver = [this](const std::string& path) -> uint32_t {
            return _renderer->getTextureId(path);
        };

        // Create multiple meshing workers for parallel processing
        // Each worker gets its OWN queue to eliminate contention
        _meshingThreads.reserve(NUM_MESHING_WORKERS);
        for (int i = 0; i < NUM_MESHING_WORKERS; ++i) {
            _meshingThreads.push_back(std::make_unique<MeshingThread>(
                _meshingTaskQueue, *_perThreadMeshQueues[i], _meshingCompleteQueue, *_blockRegistry,
                textureResolver));
        }

        _worldManager->start();
        for (auto& worker : _meshingThreads) {
            worker->start();
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to create window: " << e.what() << "\n";
        throw;
    }
}

App::~App() {
    if (_window) {
        SDL_HideWindow(_window->getSDLWindow());
    }

    for (auto& worker : _meshingThreads) {
        if (worker) {
            worker->stop();
        }
    }

    if (_worldManager) {
        _worldManager->stop();
    }

    _renderer.reset();
    _window.reset();
}

void App::run() {
    ZoneScoped;

    if (!_window) {
        std::cerr << "Window not initialized!\n";
        return;
    }

    InputManager inputManager;
    SDL_Event event;

    SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), true);

    uint64_t lastTime = SDL_GetPerformanceCounter();
    const uint64_t perfFrequency = SDL_GetPerformanceFrequency();

    while (!inputManager.shouldQuit()) {
        uint64_t currentTime = SDL_GetPerformanceCounter();
        float deltaTime =
            static_cast<float>(currentTime - lastTime) / static_cast<float>(perfFrequency);
        lastTime = currentTime;

        inputManager.newFrame();

        if (inputManager.isEscapePressed()) {
            _uiMode = !_uiMode;
            SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), !_uiMode);
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                _renderer->resizeSwapchain();
            }

            if (event.type == SDL_EVENT_QUIT) {
                SDL_HideWindow(_window->getSDLWindow());
            }

            inputManager.processEvent(event);
            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        if (inputManager.isWireframeToggled()) {
            _renderer->setWireframeMode(!_renderer->isWireframeMode());
        }

        // Update camera based on input (only if not in UI mode)
        Camera& camera = _renderer->getCamera();
        if (!_uiMode) {
            inputManager.updateCamera(camera, deltaTime);
        }

        const glm::vec3 cameraPos = camera.getPosition();
        const float chunkSizeFloat = static_cast<float>(Chunk::CHUNK_SIZE);
        const int chunkX = static_cast<int>(std::floor(cameraPos.x / chunkSizeFloat));
        const int chunkY = static_cast<int>(std::floor(cameraPos.y / chunkSizeFloat));
        const int chunkZ = static_cast<int>(std::floor(cameraPos.z / chunkSizeFloat));

        const glm::ivec3 currentCenter(chunkX, chunkY, chunkZ);
        if (_needsRequestRefresh || currentCenter != _lastRequestedCenter ||
            _loadRadius != _lastLoadRadius) {
            enqueueChunkRequests(currentCenter);
        }

        _framesSinceUnloadCheck++;
        if (_framesSinceUnloadCheck >= UNLOAD_CHECK_INTERVAL) {
            _framesSinceUnloadCheck = 0;

            // Use Chebyshev distance (same as loading) for consistent behavior
            const int unloadRadius =
                static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);

            std::vector<glm::ivec3> loadedChunks = _worldManager->getLoadedChunkPositions();

            for (const glm::ivec3& chunkPos : loadedChunks) {
                const glm::ivec3 offset = chunkPos - currentCenter;

                // Use Chebyshev distance (max of absolute values) - same as loading pattern
                const int chebyshevDistance =
                    std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

                if (chebyshevDistance > unloadRadius) {
                    _worldManager->markChunkForUnload(chunkPos);
                    _requestedChunks.erase(chunkPos); // Remove from requested set
                } else if (chebyshevDistance <= _loadRadius) {
                    // Chunk is within LOAD radius (not just unload radius) - unmark if previously
                    // marked Only unmark chunks that are close enough to be actively used
                    _worldManager->unmarkChunkForUnload(chunkPos);
                }
                // Chunks in the "buffer zone" (between loadRadius and unloadRadius) stay as they
                // are
            }

            // Get current unload queue size
            auto queueStats = _worldManager->getQueueStats();
            const size_t totalMarkedForUnload = queueStats.chunksToUnloadCount;

            if (totalMarkedForUnload >= REBUILD_THRESHOLD ||
                (totalMarkedForUnload > 0 && loadedChunks.size() > 0 &&
                 static_cast<float>(totalMarkedForUnload) /
                         static_cast<float>(loadedChunks.size()) >=
                     REBUILD_PERCENTAGE)) {

                std::vector<glm::ivec3> unloadedChunks = _worldManager->unloadMarkedChunks();
                if (!unloadedChunks.empty()) {
                    // Note: This does NOT reset the mesh pool, so other chunks keep their allocations
                    _renderer->rebuildMeshPool(unloadedChunks);

                    // No need to re-mesh other chunks - they keep their existing mesh allocations
                }
            }
        }

        _timeOfDay += _timeSpeed * deltaTime;
        if (_timeOfDay > 1.0F) {
            _timeOfDay -= 1.0F;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Debug Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS: %.1f", _renderer->getFPS());
        ImGui::Text("Frame Time: %.3f ms", deltaTime * 1000.0F);
        ImGui::Separator();

        ImGui::Text("Workers: %d meshing threads", NUM_MESHING_WORKERS);

        auto queueStats = _worldManager->getQueueStats();
        ImGui::Separator();
        ImGui::Text("Queue Statistics:");
        ImGui::Text("  Request Queue: %zu", queueStats.requestQueueSize);
        ImGui::Text("  Meshing Queue: %zu", queueStats.meshingQueueSize);
        ImGui::Text("  Loaded Chunks (RAM): %zu", queueStats.loadedChunksCount);
        ImGui::Text("  Loaded Chunks (VRAM): %zu", _renderer->getLoadedChunkCount());

        // Display mesh pool usage with color coding
        float poolUsage = _renderer->getMeshPoolUsage();
        ImVec4 usageColor = poolUsage < 0.7f   ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)  // Green < 70%
                            : poolUsage < 0.9f ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f)  // Yellow 70-90%
                                               : ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red > 90%
        ImGui::TextColored(usageColor, "  Mesh Pool Usage: %.1f%%", poolUsage * 100.0f);

        ImGui::Text("  Generating: %zu", queueStats.generatingChunksCount);
        ImGui::Text("  Meshing: %zu", queueStats.meshingChunksCount);
        ImGui::Text("  Requested: %zu", _requestedChunks.size());
        ImGui::Text("  Marked for Unload: %zu", queueStats.chunksToUnloadCount);
        ImGui::Separator();

        bool wireframeMode = _renderer->isWireframeMode();
        if (ImGui::Checkbox("Wireframe Mode (F1)", &wireframeMode)) {
            _renderer->setWireframeMode(wireframeMode);
        }

        if (ImGui::SliderInt("Chunk Load Radius", &_loadRadius, 1, 64)) {
            _needsRequestRefresh = true;
        }

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

        int hours = static_cast<int>(_timeOfDay * 24.0F);
        int minutes = static_cast<int>((_timeOfDay * 24.0F - hours) * 60.0F);
        ImGui::Text("Current Time: %02d:%02d", hours, minutes);

        auto currentChunk = _worldManager->getCurrentChunk(cameraPos);
        auto currentBiomeData =
            (currentChunk != nullptr)
                ? currentChunk->getBiomeDataAt(WORLD_TO_CHUNK(static_cast<int>(cameraPos[0])),
                                               WORLD_TO_CHUNK(static_cast<int>(cameraPos[2])))
                : BiomeType::NONE;
        std::string biomeName;
        switch (currentBiomeData) {
        case BiomeType::PLAINS:
            biomeName = "Plains";
            break;
        case BiomeType::MOUNTAINS:
            biomeName = "Mountains";
            break;
        case BiomeType::OCEAN:
            biomeName = "Ocean";
            break;
        case BiomeType::NONE:
            biomeName = "skill issue";
            break;
        default:
            biomeName = "GOUGOUGAGAK";
            break;
        }
        ImGui::Text("Biome: %s", biomeName.c_str());

        ImGui::Separator();
        if (ImGui::Button("Quit")) {
            inputManager.setShouldQuit(true);
        }

        ImGui::End();

        ImGui::Render();

        const int acceptDistance =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        _renderer->updateMeshes(currentCenter, acceptDistance);

        _renderer->updateFPS(deltaTime);

        _renderer->draw(_timeOfDay);

        FrameMark;
    }
}

void App::enqueueChunkRequests(const glm::ivec3& centerChunk) {
    if (centerChunk != _lastRequestedCenter) {
        _currentShellRadius = 0;

        const int unloadRadius =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        std::unordered_set<glm::ivec3> chunksToRemove;

        for (const glm::ivec3& requestedPos : _requestedChunks) {
            const glm::ivec3 offset = requestedPos - centerChunk;
            const int chebyshevDistance =
                std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

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

    int requestsThisFrame = 0;

    for (const glm::ivec3& offset : _chunkRequestOffsets) {
        int shellRadius = std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});

        if (shellRadius < _currentShellRadius) {
            continue;
        }

        if (shellRadius > _currentShellRadius) {
            if (requestsThisFrame >= MAX_REQUESTS_PER_FRAME) {
                break;
            }
            _currentShellRadius = shellRadius;
        }

        const glm::ivec3 chunkPos = centerChunk + offset;

        const glm::ivec3 distCheck = chunkPos - centerChunk;
        const int currentDistance =
            std::max({std::abs(distCheck.x), std::abs(distCheck.y), std::abs(distCheck.z)});

        if (currentDistance > _loadRadius) {
            continue;
        }

        if (_requestedChunks.find(chunkPos) != _requestedChunks.end()) {
            continue;
        }

        if (chunkPos[1] >= 0) {
            // CRITICAL FIX: Only unmark from unload queue if chunk is NOT already loaded
            if (!_worldManager->isChunkLoaded(chunkPos)) {
                _worldManager->unmarkChunkForUnload(chunkPos);

                _chunkRequestQueue.push(ChunkRequest(chunkPos));
                requestsThisFrame++;
            }

            // Mark as requested regardless (tracks both loaded and in-queue chunks)
            _requestedChunks.insert(chunkPos);
        }

        if (requestsThisFrame >= MAX_REQUESTS_PER_FRAME) {
            break;
        }
    }

    _lastRequestedCenter = centerChunk;
    _needsRequestRefresh = false;
}

void App::rebuildChunkOffsets(int radius) {
    _chunkRequestOffsets.clear();

    const int range = radius;

    _chunkRequestOffsets.reserve(static_cast<std::size_t>(
        (2 * range + 1) * (2 * range + 1) * (VERTICAL_CHUNK_MAX - VERTICAL_CHUNK_MIN + 1)));

    for (int dx = -range; dx <= range; ++dx) {
        for (int dy = VERTICAL_CHUNK_MIN; dy <= VERTICAL_CHUNK_MAX; ++dy) {
            for (int dz = -range; dz <= range; ++dz) {
                _chunkRequestOffsets.emplace_back(dx, dy, dz);
            }
        }
    }

    auto shellRadius = [](const glm::ivec3& offset) {
        return std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)});
    };

    auto squaredDistance = [](const glm::ivec3& offset) {
        return (offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z);
    };

    std::sort(_chunkRequestOffsets.begin(), _chunkRequestOffsets.end(),
              [&shellRadius, &squaredDistance](const glm::ivec3& a, const glm::ivec3& b) {
                  const int shellA = shellRadius(a);
                  const int shellB = shellRadius(b);
                  if (shellA != shellB) {
                      return shellA < shellB;
                  }
                  return squaredDistance(a) < squaredDistance(b);
              });
}

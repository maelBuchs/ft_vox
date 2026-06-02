#include "App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_oldnames.h>
#include <tracy/Tracy.hpp>

#include "client/Game/Camera.hpp"
#include "client/Graphics/Core/VulkanDevice.hpp"
#include "client/Graphics/Renderer.hpp"
#include "common/Util/Util.hpp"
#include "common/World/BlockRegistry.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "InputManager.hpp"
#include "server/World/MeshingThread.hpp"
#include "server/World/WorldManager.hpp"
#include "Window.hpp"

const double TPS = 60.0;
const double TIME_PER_TICK = 1.0 / TPS;

App::App() {
    try {
        _blockRegistry = std::make_unique<BlockRegistry>();
        _window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
        _vulkanDevice = std::make_unique<VulkanDevice>(_window->getSDLWindow());

        _perThreadMeshQueues.reserve(NUM_MESHING_WORKERS);
        for (int i = 0; i < NUM_MESHING_WORKERS; ++i) {
            _perThreadMeshQueues.push_back(std::make_unique<ThreadSafeQueue<MeshData>>());
        }

        _worldManager = std::make_unique<WorldManager>(_chunkRequestQueue, _meshingTaskQueue,
                                                       _meshingCompleteQueue);
        _renderer = std::make_unique<Renderer>(*_window, *_vulkanDevice, *_blockRegistry,
                                               _perThreadMeshQueues, _worldManager.get());

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

void App::updateUI(Renderer& renderer, InputManager& inputManager, Camera& camera,
                   const glm::ivec3& currentCenter, std::shared_ptr<Chunk> currentChunk) {

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Debug Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Frame Time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Separator();

    ImGui::Text("Workers: %d meshing threads", NUM_MESHING_WORKERS);

    auto queueStats = _worldManager->getQueueStats();
    ImGui::Separator();
    ImGui::Text("Queue Statistics:");
    ImGui::Text("  Request Queue: %zu", queueStats.requestQueueSize);
    ImGui::Text("  Meshing Queue: %zu", queueStats.meshingQueueSize);
    ImGui::Text("  Loaded Chunks (RAM): %zu", queueStats.loadedChunksCount);
    ImGui::Text("  Loaded Chunks (VRAM): %zu", renderer.getLoadedChunkCount());

    // Display mesh pool usage with color coding
    float poolUsage = renderer.getMeshPoolUsage();
    ImVec4 usageColor;
    if (poolUsage < 0.7F) {
        usageColor = ImVec4(0.0F, 1.0F, 0.0F, 1.0F); // Green < 70%
    } else if (poolUsage < 0.9F) {
        usageColor = ImVec4(1.0F, 1.0F, 0.0F, 1.0F); // Yellow 70-90%
    } else {
        usageColor = ImVec4(1.0F, 0.2F, 0.2F, 1.0F); // Red >= 90%
    }
    ImGui::TextColored(usageColor, "  Mesh Pool Usage: %.1f%%", poolUsage * 100.0F);
    ImGui::Text("  Generating: %zu", queueStats.generatingChunksCount);
    ImGui::Text("  Meshing: %zu", queueStats.meshingChunksCount);
    ImGui::Text("  Requested: %zu", _requestedChunks.size());
    ImGui::Text("  Marked for Unload: %zu", queueStats.chunksToUnloadCount);

    ImGui::Separator();
    ImGui::Text("LOD Debug (resident/submitted/remeshed):");
    ImGui::Text("  LOD0: %u / %u / %u", renderer.getLodResidentCount(0),
                renderer.getLodSubmittedCount(0), renderer.getLodMeshUpdateCount(0));
    ImGui::Text("  LOD1: %u / %u / %u", renderer.getLodResidentCount(1),
                renderer.getLodSubmittedCount(1), renderer.getLodMeshUpdateCount(1));
    ImGui::Text("  LOD2+: %u / %u / %u", renderer.getLodResidentCount(2),
                renderer.getLodSubmittedCount(2), renderer.getLodMeshUpdateCount(2));
    ImGui::Text("  LOD transitions this frame: +%u / -%u", renderer.getLodUpgradeCount(),
                renderer.getLodDowngradeCount());
    ImGui::Separator();

    bool wireframeMode = renderer.isWireframeMode();
    if (ImGui::Checkbox("Wireframe Mode (F1)", &wireframeMode)) {
        renderer.setWireframeMode(wireframeMode);
    }

    if (ImGui::SliderInt("Chunk Load Radius", &_loadRadius, 1, 64)) {
        _needsRequestRefresh = true;
    }

    ImGui::SliderFloat("Unload Distance Multiplier", &_unloadDistanceMultiplier, 1.5F, 4.0F);
    const int current_unload_radius =
        static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
    ImGui::Text("  Unload Radius: %d chunks (buffer: %d)", current_unload_radius,
                current_unload_radius - _loadRadius);

    ImGui::Separator();
    const glm::vec3 camPos = camera.getPosition();
    ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", static_cast<double>(camPos[0]),
                static_cast<double>(camPos[1]), static_cast<double>(camPos[2]));
    ImGui::Text("Chunk: (%d, %d, %d)", worldToChunk(static_cast<int>(camPos[0])),
                worldToChunk(static_cast<int>(camPos[1])),
                worldToChunk(static_cast<int>(camPos[2])));
    auto chunkPos = currentChunk ? currentChunk->getPosition() : glm::ivec3(0, 0, 0);
    ImGui::Text("Chunk: (%d, %d, %d)", chunkPos[0], chunkPos[1], chunkPos[2]);
    ImGui::Text("Local Pos: (%d, %d, %d)", worldToBlock(static_cast<int>(camPos[0])),
                worldToBlock(static_cast<int>(camPos[1])),
                worldToBlock(static_cast<int>(camPos[2])));
    const float cam_yaw = camera.getYaw();
    const float cam_pitch = camera.getPitch();
    ImGui::Text("Camera Rotation: (Pitch: %.1f, Yaw: %.1f)", cam_pitch, cam_yaw);

    ImGui::Separator();
    ImGui::Text("Sky System");
    if (ImGui::SliderFloat("Time of Day", &_timeOfDay, 0.0F, 1.0F)) {
        // Slider changed, time is now manually controlled
    }
    ImGui::SliderFloat("Time Speed", &_timeSpeed, 0.0F, 0.2F);

    auto current_biome_data = (currentChunk != nullptr) ? BiomeType::kPLAINS : BiomeType::kNONE;
    auto currentBiomeData = (currentChunk != nullptr);
    // ? currentChunk.getBiomeDataAt(worldToChunk(static_cast<int>(cameraPos[0])),
    // std::string biome_name;
    // switch (current_biome_data) {
    // case BiomeType::kPLAINS:
    //     biome_name = "Plains";
    //     break;
    // case BiomeType::kMOUNTAINS:
    //     biome_name = "Mountains";
    //     break;
    // case BiomeType::kOCEAN:
    //     biome_name = "Ocean";
    //     break;
    // case BiomeType::kNONE:
    //     biome_name = "Unknown";
    //     break;
    // default:
    //     biome_name = "Unknown";
    if (currentChunk != nullptr) {
        auto current_block_noise = currentChunk->getNoiseParams(
            worldToChunk(static_cast<int>(camPos[0])), worldToChunk(static_cast<int>(camPos[2])));
        std::array<char, 256> noise_text{};

        std::snprintf(noise_text.data(), noise_text.size(),
                      "T = %.3f, H = %.3f, C = %.3f, E = %.3f, W = %.3f, D = %.3f",
                      current_block_noise.kTEMPERATURE, current_block_noise.humidity,
                      current_block_noise.continent, current_block_noise.erosion,
                      current_block_noise.weirdness, current_block_noise.depth);
        ImGui::TextUnformatted(noise_text.data());
    } else {
        ImGui::TextUnformatted("Out Of Boundaries");
    }
    ImGui::Separator();

    // Chunk data
    if (currentChunk != nullptr) {
        glm::ivec3 blockPos = camera.getPosition() + camera.getFront() * 5.0F;
        blockPos.x = worldToBlock(floor(blockPos.x));
        blockPos.y = worldToBlock(floor(blockPos.y));
        blockPos.z = worldToBlock(floor(blockPos.z));
        uint8_t blockId = currentChunk->getBlock(blockPos.x, blockPos.y, blockPos.z);
        std::string blockName = _blockRegistry->getBlockName(static_cast<int>(blockId));
        ImGui::Text("Current Block: ID %d (%s) at Local Pos (%d, %d, %d)", blockId,
                    blockName.c_str(), blockPos.x, blockPos.y, blockPos.z);
    } else {
        ImGui::TextUnformatted("No Chunk Loaded at Camera Position");
    }

    ImGui::Separator();
    if (ImGui::Button("Quit")) {
        inputManager.setShouldQuit(true);
    }
    ImGui::End();
    ImGui::EndFrame();
    ImGui::Render();
}

void App::updateRender(Camera& camera, InputManager& inputManager) {

    const glm::vec3 cameraPos = camera.getPosition();
    const float chunkSizeFloat = static_cast<float>(Chunk::CHUNK_SIZE);
    const int chunkX = worldToChunk(static_cast<int>(cameraPos.x));
    const int chunkY = worldToChunk(static_cast<int>(cameraPos.y));
    const int chunkZ = worldToChunk(static_cast<int>(cameraPos.z));

    const glm::ivec3 currentCenter(chunkX, chunkY, chunkZ);
    bool stillLoadingShells = (_currentShellRadius < _loadRadius);
    if (_needsRequestRefresh || currentCenter != _lastRequestedCenter ||
        _loadRadius != _lastLoadRadius || stillLoadingShells) {
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
             static_cast<float>(totalMarkedForUnload) / static_cast<float>(loadedChunks.size()) >=
                 REBUILD_PERCENTAGE)) {

            std::vector<glm::ivec3> unloadedChunks = _worldManager->unloadMarkedChunks();
            if (!unloadedChunks.empty()) {
                // Note: This does NOT reset the mesh pool, so other chunks keep their
                // allocations
                _renderer->rebuildMeshPool(unloadedChunks);

                // No need to re-mesh other chunks - they keep their existing mesh allocations
            }
        }
    }

    updateUI(*_renderer, inputManager, camera, currentCenter,
             _worldManager->getChunkAtPosition(currentCenter));

    const int acceptDistance =
        static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
    _renderer->updateMeshes(currentCenter, acceptDistance);

    _renderer->draw(_timeOfDay);
}

void App::manageInputs(InputManager& inputManager, Window& window, SDL_Event event) {
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            int w, h;
            SDL_GetWindowSizeInPixels(window.getSDLWindow(), &w, &h);
            window.updateSize(w, h);
            _renderer->resizeSwapchain();
        }

        if (event.type == SDL_EVENT_QUIT) {
            SDL_HideWindow(window.getSDLWindow());
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            switch (inputManager.mouseInput(event)) {
            case 1: {
                auto block = _worldManager->getTargetBlock(_renderer->getCamera());
                if (block.has_value()) {
                    _worldManager->setBlockValue(block.value(), 0);
                    glm::ivec3 chunkPos = {worldToChunk(static_cast<int>(block.value()[0])),
                                           worldToChunk(static_cast<int>(block.value()[1])),
                                           worldToChunk(static_cast<int>(block.value()[2]))};
                    _worldManager->updatedBlockAt(block.value());
                }
                break;
            }
            case 2: {
                auto block2 = _worldManager->getTargetBlock(_renderer->getCamera());
                if (block2.has_value()) {
                    _worldManager->setBlockValue(block2.value(), 1);
                    glm::ivec3 chunkPos = {worldToChunk(static_cast<int>(block2.value()[0])),
                                           worldToChunk(static_cast<int>(block2.value()[1])),
                                           worldToChunk(static_cast<int>(block2.value()[2]))};
                    _worldManager->updatedBlockAt(block2.value());
                }
                break;
            }
            }
        }
        inputManager.processEvent(event);
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
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

    // uint64_t lastTime = SDL_GetPerformanceCounter();
    // const uint64_t perfFrequency = SDL_GetPerformanceFrequency();

    auto lastTime = std::chrono::steady_clock::now();
    double accumulator = 0.0;

    while (!inputManager.shouldQuit()) {

        inputManager.newFrame();
        inputManager.setMouseDelta(glm::vec2(0.0F, 0.0F));
        Camera& camera = _renderer->getCamera();
        auto currentTime = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = currentTime - lastTime;
        lastTime = currentTime;
        double frameTime = elapsed.count();
        if (frameTime > 0.25) {
            frameTime = 0.25; // Cap to avoid spiral of death
            std::cout << "Long frame time detected: " << frameTime << " seconds\n";
        }
        accumulator += frameTime;
        if (inputManager.isEscapePressed()) {
            _uiMode = !_uiMode;
            SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), !_uiMode);
        }
        manageInputs(inputManager, *_window, event);
        inputManager.updateCameraRotation(camera);
        while (accumulator >= TIME_PER_TICK) {
            // LOGIC UPDATE GOES HERE
            if (inputManager.isWireframeToggled()) {
                _renderer->setWireframeMode(!_renderer->isWireframeMode());
            }

            if (!_uiMode) {
                inputManager.updateCamera(camera, TIME_PER_TICK);
            }
            accumulator -= TIME_PER_TICK;
        }

        // double alpha = accumulator / TIME_PER_TICK;
        // if u want to interpolate

        updateRender(camera, inputManager);

        FrameMark;
    }
}

void App::enqueueChunkRequests(const glm::ivec3& centerChunk) {
    auto computeRequestLod = [this](int chebyshevDistance) -> uint32_t {
        const int lod1Start = std::max(6, _loadRadius / 3);
        const int lod2Start = std::max(lod1Start + 4, (_loadRadius * 2) / 3);
        if (chebyshevDistance >= lod2Start) {
            return 2;
        }
        if (chebyshevDistance >= lod1Start) {
            return 1;
        }
        return 0;
    };

    if (centerChunk != _lastRequestedCenter) {
        _currentShellRadius = 0;

        const int unloadRadius =
            static_cast<int>(static_cast<float>(_loadRadius) * _unloadDistanceMultiplier);
        std::unordered_set<glm::ivec3> chunksToRemove;

        for (const auto& [requestedPos, lodLevel] : _requestedChunks) {
            static_cast<void>(lodLevel);
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

        const uint32_t requestedLod = computeRequestLod(currentDistance);
        auto requestedIt = _requestedChunks.find(chunkPos);
        if (requestedIt != _requestedChunks.end() && requestedIt->second == requestedLod) {
            continue;
        }

        if (chunkPos[1] >= 0) {
            _worldManager->unmarkChunkForUnload(chunkPos);
            _chunkRequestQueue.push(ChunkRequest(chunkPos, requestedLod));
            requestsThisFrame++;

            _requestedChunks[chunkPos] = requestedLod;
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

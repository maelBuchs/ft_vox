#include "App.hpp"

#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

#include "client/Graphics/Renderer.hpp"
#include "client/Graphics/VulkanDevice.hpp"
#include "common/World/BlockRegistry.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "Window.hpp"

App::App() {
    try {
        _blockRegistry = std::make_unique<BlockRegistry>();
        _window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
        _vulkanDevice = std::make_unique<VulkanDevice>(_window->getSDLWindow());
        _renderer = std::make_unique<Renderer>(*_window, *_vulkanDevice, *_blockRegistry);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create window: " << e.what() << "\n";
        throw;
    }
}

App::~App() {}

void App::run() {
    if (!_window) {
        std::cerr << "Window not initialized!\n";
        return;
    }
    SDL_Event event;
    bool running = true;

    SDL_SetWindowRelativeMouseMode(_window->getSDLWindow(), true);
    std::cout << "[APP] Camera controls: WASD to move, Mouse to look, ESC to quit\n";
    std::cout << "[APP] Press F1 to toggle wireframe mode\n";

    // Delta time tracking
    uint64_t lastTime = SDL_GetPerformanceCounter();
    const uint64_t perfFrequency = SDL_GetPerformanceFrequency();

    while (running) {
        // Calculate delta time
        uint64_t currentTime = SDL_GetPerformanceCounter();
        float deltaTime =
            static_cast<float>(currentTime - lastTime) / static_cast<float>(perfFrequency);
        lastTime = currentTime;

        while (SDL_PollEvent(&event)) {
            if ((event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) ||
                (event.type == SDL_EVENT_QUIT)) {
                running = false;
            }

            // Toggle wireframe mode with F1
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1) {
                _renderer->setWireframeMode(!_renderer->isWireframeMode());
            }

            // Handle window resize
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                _renderer->resizeSwapchain();
            }

            _renderer->processInput(event);

            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ImGui UI for FPS and wireframe toggle
        ImGui::Begin("Debug Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS: %.1f", _renderer->getFPS());
        ImGui::Text("Frame Time: %.3f ms", deltaTime * 1000.0f);
        ImGui::Separator();

        bool wireframeMode = _renderer->isWireframeMode();
        if (ImGui::Checkbox("Wireframe Mode (F1)", &wireframeMode)) {
            _renderer->setWireframeMode(wireframeMode);
        }

        ImGui::Separator();
        ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", 0.0f, 0.0f, 0.0f);
        ImGui::End();

        ImGui::Render();

        // Update FPS counter
        _renderer->updateFPS(deltaTime);

        // Update camera
        _renderer->updateCamera(deltaTime);

        _renderer->draw();
    }
}

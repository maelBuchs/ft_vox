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

    while (running) {
        while (SDL_PollEvent(&event)) {
            if ((event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) ||
                (event.type == SDL_EVENT_QUIT)) {
                running = false;
            }
            _renderer->processInput(event);

            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        if (_renderer->isResizeRequested()) {
            _renderer->resizeSwapchain();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // TODO: Add your ImGui UI here (FPS counter, debug info, etc.)

        ImGui::Render();

        // Update camera (deltaTime = 0.016 for ~60fps)
        _renderer->updateCamera(0.016F);

        _renderer->draw();
    }
}

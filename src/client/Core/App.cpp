#include "App.hpp"

#include <iostream>
#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

#include "client/Graphics/Renderer.hpp"
#include "client/Graphics/VulkanDevice.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "Window.hpp"

App::App() {
    try {
        _window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
        _vulkanDevice = std::make_unique<VulkanDevice>(_window->getSDLWindow());
        _renderer = std::make_unique<Renderer>(*_window, *_vulkanDevice);
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

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
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

        _renderer->draw();
    }
}

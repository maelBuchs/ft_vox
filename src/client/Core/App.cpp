#include "App.hpp"

#include <iostream>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

#include "Window.hpp"

App::App() {
    try {
        window = std::make_unique<Window>(WIDTH, HEIGHT, WINDOW_TITLE);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create window: " << e.what() << "\n";
        throw;
    }
}

App::~App() {}

void App::run() {
    if (!window) {
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
        }
    }
}

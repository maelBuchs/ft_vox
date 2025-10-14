#include "Window.hpp"

#include <iostream>
#include <utility>

#include <SDL3/SDL_init.h>

Window::Window(int w, int h, std::string name) : width(w), height(h), windowName(std::move(name)) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
        exit(-1);
    }
    window = SDL_CreateWindow(windowName.c_str(), width, height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << "\n";
        SDL_Quit();
        throw std::runtime_error("Failed to create SDL window");
    }
}

Window::~Window() {
    SDL_DestroyWindow(window);
    SDL_Quit();
}

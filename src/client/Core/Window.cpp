#include "Window.hpp"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <iostream>

Window::Window(int w, int h, std::string name)
    : width(w), height(h), windowName(name) {
  initWindow();
}

void Window::initWindow() {

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
    exit(-1);
  }
  window = SDL_CreateWindow("Vulkan window", 800, 600,
                            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

  if (!window) {
    std::cerr << "Failed to create SDL window: " << SDL_GetError() << "\n";
    SDL_Quit();
    exit(-1); // TODO - cleanup "exit -1"
  }
}

Window::~Window() {
  SDL_DestroyWindow(window);
  SDL_Quit();
}

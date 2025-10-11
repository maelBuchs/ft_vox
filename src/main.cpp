#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_core.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <iostream>

int main() {

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
    return -1;
  }

  SDL_Window *window =
      SDL_CreateWindow("Vulkan window", 800, 600, SDL_WINDOW_VULKAN);

  if (!window) {
    std::cerr << "Failed to create SDL window: " << SDL_GetError() << "\n";
    SDL_Quit();
    return -1;
  }

  uint32_t extensionCount = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

  std::cout << extensionCount << " extensions supported\n";

  glm::mat4 matrix;
  glm::vec4 vec;
  auto test = matrix * vec;

  bool running = true;
  SDL_Event event;
  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT)
        running = false;
    }
    SDL_Delay(16);
  }

  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}

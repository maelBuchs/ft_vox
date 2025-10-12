#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <string>
#include <vulkan/vulkan.h>

class Window {
public:
  Window(int w, int h, std::string name);
  ~Window();

  void setShouldClose(bool i) { _shouldClose = i; }
  bool shouldClose() { return _shouldClose; }
  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;

private:
  void initWindow();

  const int width;
  const int height;
  bool _shouldClose = false;
  std::string windowName;
  SDL_Window *window;
};
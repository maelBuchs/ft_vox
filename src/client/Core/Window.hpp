#pragma once

#include <string>

struct SDL_Window;

class Window {
  public:
    Window(int w, int h, std::string name);
    ~Window();

    [[nodiscard]] SDL_Window* getSDLWindow() const { return window; }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

  private:
    const int width;
    const int height;
    std::string windowName;
    SDL_Window* window;
};

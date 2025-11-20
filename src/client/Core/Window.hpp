#pragma once

#include <string>

struct SDL_Window;

class Window {
  public:
    Window(int w, int h, std::string name);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] SDL_Window* getSDLWindow() const { return window; }
    [[nodiscard]] int getWidth() const { return width; }
    [[nodiscard]] int getHeight() const { return height; }
    [[nodiscard]] const std::string& getWindowName() const { return windowName; }
    void updateSize(int w, int h) { width = w; height = h; }

  private:
    int width;
    int height;
    std::string windowName;
    SDL_Window* window;
};

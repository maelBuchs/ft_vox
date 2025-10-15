#pragma once

#include <memory>

class Window;
class VulkanDevice;

class App {
  public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    static constexpr int WIDTH = 800;
    static constexpr int HEIGHT = 600;
    static constexpr const char* WINDOW_TITLE = "Vulkan App";

    void run();

  private:
    std::unique_ptr<Window> _window;
    std::unique_ptr<VulkanDevice> _vulkanDevice;
};

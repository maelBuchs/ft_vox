#pragma once

#include <memory>

class Window;
class VulkanDevice;
class Renderer;
class BlockRegistry;

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

    [[nodiscard]] float getTimeOfDay() const { return _timeOfDay; }
    void setTimeOfDay(float time) { _timeOfDay = time; }

  private:
    std::unique_ptr<BlockRegistry> _blockRegistry;
    std::unique_ptr<Window> _window;
    std::unique_ptr<VulkanDevice> _vulkanDevice;
    std::unique_ptr<Renderer> _renderer;

    // Time of day system (0.0 = midnight, 0.5 = noon, 1.0 = midnight)
    float _timeOfDay = 0.5F; // Start at noon
    float _timeSpeed = 0.02F; // Speed of time progression (adjustable)
};

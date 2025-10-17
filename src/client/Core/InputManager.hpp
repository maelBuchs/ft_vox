#pragma once

#include <SDL3/SDL_events.h>

#include <glm/glm.hpp>

class Camera;

class InputManager {
  public:
    InputManager() = default;
    ~InputManager() = default;

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;
    InputManager(InputManager&&) = delete;
    InputManager& operator=(InputManager&&) = delete;

    // Process SDL events
    void processEvent(const SDL_Event& event);
    void newFrame(); // Call at start of frame to reset per-frame state

    // Keyboard state
    [[nodiscard]] bool isKeyPressed(SDL_Scancode key) const;
    [[nodiscard]] bool isKeyDown(SDL_Scancode key) const;

    // Special actions
    [[nodiscard]] bool shouldQuit() const { return _shouldQuit; }
    [[nodiscard]] bool isWireframeToggled();

    // Mouse state
    [[nodiscard]] glm::vec2 getMouseDelta() const { return _mouseDelta; }
    [[nodiscard]] bool isMouseButtonPressed(int button) const;

    // Camera update
    void updateCamera(Camera& camera, float deltaTime);

  private:
    bool _shouldQuit = false;
    bool _wireframeToggled = false;
    bool _wireframeKeyWasPressed = false;
    glm::vec2 _mouseDelta{0.0F, 0.0F};
};

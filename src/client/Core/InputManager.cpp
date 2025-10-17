#include "InputManager.hpp"

#include <SDL3/SDL.h>

#include "client/Game/Camera.hpp"

void InputManager::processEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) {
        _shouldQuit = true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        _shouldQuit = true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1) {
        if (!_wireframeKeyWasPressed) {
            _wireframeToggled = true;
            _wireframeKeyWasPressed = true;
        }
    }

    if (event.type == SDL_EVENT_KEY_UP && event.key.key == SDLK_F1) {
        _wireframeKeyWasPressed = false;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        _mouseDelta.x = static_cast<float>(event.motion.xrel);
        _mouseDelta.y = static_cast<float>(event.motion.yrel);
    }
}

void InputManager::newFrame() {
    _mouseDelta = glm::vec2(0.0F, 0.0F);
}

bool InputManager::isKeyPressed(SDL_Scancode key) const {
    const bool* keystate = SDL_GetKeyboardState(nullptr);
    if (keystate == nullptr) {
        return false;
    }
    return keystate[key];
}

bool InputManager::isKeyDown(SDL_Scancode key) const {
    return isKeyPressed(key);
}

bool InputManager::isWireframeToggled() {
    if (_wireframeToggled) {
        _wireframeToggled = false;
        return true;
    }
    return false;
}

bool InputManager::isMouseButtonPressed(int button) const {
    return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
}

void InputManager::updateCamera(Camera& camera, float deltaTime) {
    // Process mouse movement
    if (_mouseDelta.x != 0.0F || _mouseDelta.y != 0.0F) {
        camera.processMouseMovement(_mouseDelta.x, -_mouseDelta.y);
    }

    // Process keyboard movement
    if (isKeyPressed(SDL_SCANCODE_W)) {
        camera.processKeyboard(CameraMovement::Forward, deltaTime);
    }
    if (isKeyPressed(SDL_SCANCODE_S)) {
        camera.processKeyboard(CameraMovement::Backward, deltaTime);
    }
    if (isKeyPressed(SDL_SCANCODE_A)) {
        camera.processKeyboard(CameraMovement::Left, deltaTime);
    }
    if (isKeyPressed(SDL_SCANCODE_D)) {
        camera.processKeyboard(CameraMovement::Right, deltaTime);
    }
    if (isKeyPressed(SDL_SCANCODE_SPACE)) {
        camera.processKeyboard(CameraMovement::Up, deltaTime);
    }
    if (isKeyPressed(SDL_SCANCODE_LSHIFT)) {
        camera.processKeyboard(CameraMovement::Down, deltaTime);
    }
}

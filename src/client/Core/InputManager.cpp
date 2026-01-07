#include "InputManager.hpp"

#include <imgui_internal.h>
#include <iostream>

#include <SDL3/SDL.h>
#include <SDL3/SDL_mouse.h>

#include "client/Game/Camera.hpp"

void InputManager::processEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) {
        _shouldQuit = true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        _escapePressed = true;
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
        _mouseDelta.x += static_cast<float>(event.motion.xrel);
        _mouseDelta.y += static_cast<float>(event.motion.yrel);
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

bool InputManager::isWireframeToggled() {
    if (_wireframeToggled) {
        _wireframeToggled = false;
        return true;
    }
    return false;
}

bool InputManager::isEscapePressed() {
    if (_escapePressed) {
        _escapePressed = false;
        return true;
    }
    return false;
}

bool InputManager::isMouseButtonPressed(int button) const {
    return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
}

void InputManager::updateCameraRotation(Camera& camera) {
    if (_mouseDelta.x != 0.0F || _mouseDelta.y != 0.0F) {
        camera.processMouseMovement(_mouseDelta.x, -_mouseDelta.y);
    }
}

void InputManager::updateCamera(Camera& camera, float deltaTime) {
    // Process mouse movement

    // Process keyboard movement
    if (isKeyPressed(SDL_SCANCODE_LALT)) {
        camera.setSpeed(5.0F);
    } else {
        camera.setSpeed(1.0F);
    }

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

int InputManager::mouseInput(SDL_Event& event) {
    if (event.button.button == SDL_BUTTON_LEFT) {
        return 1;
    }
    if (event.button.button == SDL_BUTTON_RIGHT) {
        return 2;
    }
    return 0;
}

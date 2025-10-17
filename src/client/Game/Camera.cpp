#include "Camera.hpp"

#include <algorithm>
#include <cmath>

Camera::Camera(glm::vec3 position, float yaw, float pitch)
    : _position(position), _yaw(yaw), _pitch(pitch), _worldUp(0.0F, 1.0F, 0.0F),
      _speed(DEFAULT_SPEED), _sensitivity(DEFAULT_SENSITIVITY) {
    updateCameraVectors();
}

void Camera::processMouseMovement(float xoffset, float yoffset) {
    _yaw += xoffset * _sensitivity;
    _pitch += yoffset * _sensitivity;

    // Constrain pitch to prevent screen flip
    _pitch = std::clamp(_pitch, -MAX_PITCH, MAX_PITCH);

    // Update Front, Right and Up Vectors using the updated Euler angles
    updateCameraVectors();
}

void Camera::processKeyboard(CameraMovement direction, float deltaTime) {
    float velocity = _speed * deltaTime * 60.0F; // Normalize for 60fps

    switch (direction) {
    case CameraMovement::Forward:
        _position += _front * velocity;
        break;
    case CameraMovement::Backward:
        _position -= _front * velocity;
        break;
    case CameraMovement::Left:
        _position -= glm::normalize(glm::cross(_front, _up)) * velocity;
        break;
    case CameraMovement::Right:
        _position += glm::normalize(glm::cross(_front, _up)) * velocity;
        break;
    case CameraMovement::Up:
        _position += _up * velocity;
        break;
    case CameraMovement::Down:
        _position -= _up * velocity;
        break;
    }
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(_position, _position + _front, _up);
}

void Camera::updateCameraVectors() {
    // Calculate the new Front vector
    glm::vec3 front;
    front.x = cos(glm::radians(_yaw)) * cos(glm::radians(_pitch));
    front.y = sin(glm::radians(_pitch));
    front.z = sin(glm::radians(_yaw)) * cos(glm::radians(_pitch));
    _front = glm::normalize(front);

    // Recalculate the Up vector
    _up = _worldUp;
}

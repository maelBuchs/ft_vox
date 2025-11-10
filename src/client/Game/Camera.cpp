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

    _up = _worldUp;
}

std::array<glm::vec4, 6> Camera::getFrustumPlanes(const glm::mat4& projection) const {
    // Extract frustum planes from view-projection matrix using Gribb-Hartmann method
    const glm::mat4 viewProjection = projection * getViewMatrix();
    const glm::mat4& m = viewProjection;

    std::array<glm::vec4, 6> planes;

    // Left plane: m[3] + m[0]
    planes[0] =
        glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);

    // Right plane: m[3] - m[0]
    planes[1] =
        glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);

    // Top plane: m[3] - m[1]
    planes[2] =
        glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);

    // Bottom plane: m[3] + m[1]
    planes[3] =
        glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);

    // Near plane: m[3] + m[2]
    planes[4] =
        glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);

    // Far plane: m[3] - m[2]
    planes[5] =
        glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);

    // Normalize all planes
    for (auto& plane : planes) {
        const glm::vec3 normal(plane.x, plane.y, plane.z);
        const float length = glm::length(normal);
        if (length > 0.0001f) {
            plane /= length;
        }
    }

    return planes;
}

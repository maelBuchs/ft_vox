#pragma once

#include <array>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

enum class CameraMovement { Forward, Backward, Left, Right, Up, Down };

class Camera {
  public:
    Camera(glm::vec3 position = glm::vec3(0.0F, 0.0F, 0.0F), float yaw = -90.0F,
           float pitch = 0.0F);
    ~Camera() = default;

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) = default;
    Camera& operator=(Camera&&) = default;

    // Process input
    void processMouseMovement(float xoffset, float yoffset);
    void processKeyboard(CameraMovement direction, float deltaTime);

    // Getters
    [[nodiscard]] glm::mat4 getViewMatrix() const;
    [[nodiscard]] glm::vec3 getPosition() const { return _position; }
    [[nodiscard]] glm::vec3 getFront() const { return _front; }
    [[nodiscard]] glm::vec3 getUp() const { return _up; }
    [[nodiscard]] float getYaw() const { return _yaw; }
    [[nodiscard]] float getPitch() const { return _pitch; }

    /**
     * Extract frustum planes from view-projection matrix
     * Returns 6 planes in order: left, right, top, bottom, near, far
     * Each plane is vec4(a, b, c, d) for equation ax + by + cz + d = 0
     * @param projection Projection matrix
     * @return Array of 6 frustum planes (normalized)
     */
    [[nodiscard]] std::array<glm::vec4, 6> getFrustumPlanes(const glm::mat4& projection) const;

    // Setters
    void setPosition(glm::vec3 position) { _position = position; }
    void setSpeed(float speed) { _speed = speed; }
    void setSensitivity(float sensitivity) { _sensitivity = sensitivity; }

  private:
    void updateCameraVectors();

    // Camera attributes
    glm::vec3 _position;
    glm::vec3 _front;
    glm::vec3 _up;
    glm::vec3 _worldUp;

    // Euler angles
    float _yaw;
    float _pitch;

    // Camera options
    float _speed;
    float _sensitivity;

    // Constants
    static constexpr float DEFAULT_SPEED = 1.0F;
    static constexpr float DEFAULT_SENSITIVITY = 0.5F;
    static constexpr float MAX_PITCH = 89.0F;
};

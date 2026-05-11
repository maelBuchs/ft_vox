#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @brief Centralized graphics utilities and constants.
 *
 */
namespace GraphicsUtils {

// =============================================================================
// Projection Constants
// =============================================================================

namespace Projection {
    constexpr float FOV = 80.0F;              // Field of view in degrees
    constexpr float NEAR_PLANE = 0.1F;        // Near clipping plane
    constexpr float FAR_PLANE = 10000.0F;     // Far clipping plane
    constexpr float SKY_FAR_PLANE = 1000.0F;  // Far plane for sky rendering (shorter)
}

// =============================================================================
// Chunk Rendering Constants
// =============================================================================

namespace Chunk {
    constexpr float SIZE_FLOAT = 32.0F; // Chunk size in world units (must match Chunk::CHUNK_SIZE)
}

// =============================================================================
// Shader Workgroup Constants
// =============================================================================

namespace Workgroup {
    constexpr uint32_t FRUSTUM_CULL_SIZE = 64;        // Workgroup size for frustum culling compute shader
    constexpr uint32_t TASK_SHADER_SIZE = 16;         // Chunks per task shader workgroup
    constexpr uint32_t MAX_MESH_VERTICES = 256;       // Maximum vertices per mesh shader workgroup
}

// =============================================================================
// Projection Functions
// =============================================================================

/**
 * @brief Creates a Vulkan-compatible perspective projection matrix.
 *
 * Applies the Vulkan Y-axis flip (Vulkan uses inverted Y compared to OpenGL).
 *
 * @param aspectRatio Width / Height ratio
 * @param fovDegrees Field of view in degrees (default: 80.0)
 * @param nearPlane Near clipping plane distance (default: 0.1)
 * @param farPlane Far clipping plane distance (default: 10000.0)
 * @return glm::mat4 Vulkan-compatible projection matrix with Y-axis flipped
 */
[[nodiscard]] inline glm::mat4 createVulkanProjection(
    float aspectRatio,
    float fovDegrees = Projection::FOV,
    float nearPlane = Projection::NEAR_PLANE,
    float farPlane = Projection::FAR_PLANE
) {
    glm::mat4 projection = glm::perspective(glm::radians(fovDegrees), aspectRatio, nearPlane, farPlane);
    projection[1][1] *= -1.0F; // Flip Y for Vulkan coordinate system
    return projection;
}

/**
 * @brief Calculates aspect ratio from extent dimensions.
 */
[[nodiscard]] inline float calculateAspectRatio(uint32_t width, uint32_t height) {
    return static_cast<float>(width) / static_cast<float>(height);
}

/**
 * @brief Creates a Vulkan-compatible projection matrix from extent dimensions.
 *
 * @param width
 * @param height
 * @param fovDegrees
 * @param nearPlane
 * @param farPlane
 * @return glm::mat4
 */
[[nodiscard]] inline glm::mat4 createVulkanProjectionFromExtent(
    uint32_t width,
    uint32_t height,
    float fovDegrees = Projection::FOV,
    float nearPlane = Projection::NEAR_PLANE,
    float farPlane = Projection::FAR_PLANE
) {
    return createVulkanProjection(calculateAspectRatio(width, height), fovDegrees, nearPlane, farPlane);
}

// =============================================================================
// Workgroup Calculation Functions
// =============================================================================

/**
 * @brief Calculate number of workgroups needed for frustum culling.
 */
[[nodiscard]] constexpr uint32_t calculateCullWorkgroups(uint32_t totalChunks) {
    return (totalChunks + Workgroup::FRUSTUM_CULL_SIZE - 1) / Workgroup::FRUSTUM_CULL_SIZE;
}

/**
 * @brief Calculate number of task workgroups needed for mesh shader dispatch.
 */
[[nodiscard]] constexpr uint32_t calculateTaskWorkgroups(uint32_t totalChunks) {
    return (totalChunks + Workgroup::TASK_SHADER_SIZE - 1) / Workgroup::TASK_SHADER_SIZE;
}

/**
 * @brief Calculate render distance in world units from chunk load radius.
 *
 * Chunks are loaded in a Chebyshev distance pattern (cube), but culling uses Euclidean distance.
 * For a chunk at Chebyshev distance N, the farthest point (corner chunk center) is at:
 * Euclidean distance = sqrt(3) * (N + 0.5) * chunkSize
 *
 * @param loadDistanceChunks Chunk load radius (Chebyshev distance)
 * @return Maximum render distance in world units to include all loaded chunks
 */
[[nodiscard]] constexpr float calculateRenderDistance(int loadDistanceChunks) {
    // sqrt(3) ≈ 1.732051 - diagonal distance factor for cube
    // +0.5 accounts for chunk center offset from corner
    // +0.1 adds small margin for numerical stability
    const float diagonalDistance = 1.732051F * (static_cast<float>(loadDistanceChunks) + 0.5F);
    const float margin = 0.1F;
    return (diagonalDistance + margin) * Chunk::SIZE_FLOAT;
}

}

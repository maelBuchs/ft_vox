#pragma once

#include <array>
#include <glm/glm.hpp>

/**
 * Frustum class for view frustum culling.
 * Stores 6 planes (left, right, top, bottom, near, far) and provides
 * intersection tests for frustum culling.
 */
class Frustum {
  public:
    /**
     * Plane representation: ax + by + cz + d = 0
     * Stored as vec4(a, b, c, d) where (a,b,c) is the normal pointing inward
     */
    enum class PlaneIndex : uint32_t {
        Left = 0,
        Right = 1,
        Top = 2,
        Bottom = 3,
        Near = 4,
        Far = 5
    };

    Frustum() = default;

    /**
     * Construct frustum from view-projection matrix
     * Extracts the 6 frustum planes using Gribb-Hartmann method
     * @param viewProjection Combined view-projection matrix
     */
    explicit Frustum(const glm::mat4& viewProjection);

    /**
     * Update frustum planes from view-projection matrix
     * @param viewProjection Combined view-projection matrix
     */
    void update(const glm::mat4& viewProjection);

    /**
     * Test if a sphere intersects the frustum
     * @param center Center of the sphere in world space
     * @param radius Radius of the sphere
     * @return true if sphere is at least partially inside frustum
     */
    [[nodiscard]] bool testSphere(const glm::vec3& center, float radius) const;

    /**
     * Test if an AABB intersects the frustum
     * @param minPoint Minimum corner of AABB
     * @param maxPoint Maximum corner of AABB
     * @return true if AABB is at least partially inside frustum
     */
    [[nodiscard]] bool testAABB(const glm::vec3& minPoint, const glm::vec3& maxPoint) const;

    /**
     * Get the frustum planes (read-only)
     * @return Array of 6 planes in order: left, right, top, bottom, near, far
     */
    [[nodiscard]] const std::array<glm::vec4, 6>& getPlanes() const { return _planes; }

    /**
     * Get a specific frustum plane
     * @param index Plane index (0-5)
     * @return Plane equation as vec4(a, b, c, d)
     */
    [[nodiscard]] const glm::vec4& getPlane(PlaneIndex index) const {
        return _planes[static_cast<uint32_t>(index)];
    }

    /**
     * Test if frustum is valid (all planes normalized)
     * Useful for debugging
     */
    [[nodiscard]] bool isValid() const;

  private:
    /**
     * Normalize a plane equation
     * Ensures the normal vector (a, b, c) is unit length
     */
    static void normalizePlane(glm::vec4& plane);

    /**
     * Calculate signed distance from point to plane
     * Positive = in front of plane (inside), negative = behind plane (outside)
     */
    static float distanceToPlane(const glm::vec4& plane, const glm::vec3& point);

    // 6 frustum planes: left, right, top, bottom, near, far
    // Each plane stored as vec4(a, b, c, d) for equation ax + by + cz + d = 0
    std::array<glm::vec4, 6> _planes{};
};

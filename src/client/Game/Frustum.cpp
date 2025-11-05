#include "Frustum.hpp"
#include <cmath>

Frustum::Frustum(const glm::mat4& viewProjection) {
    update(viewProjection);
}

void Frustum::update(const glm::mat4& viewProjection) {
    // Extract frustum planes using Gribb-Hartmann method
    // This extracts planes directly from the view-projection matrix
    // Each plane is extracted by adding/subtracting rows of the matrix

    const glm::mat4& m = viewProjection;

    // Left plane: m[3] + m[0]
    _planes[static_cast<uint32_t>(PlaneIndex::Left)] = glm::vec4(
        m[0][3] + m[0][0],
        m[1][3] + m[1][0],
        m[2][3] + m[2][0],
        m[3][3] + m[3][0]
    );

    // Right plane: m[3] - m[0]
    _planes[static_cast<uint32_t>(PlaneIndex::Right)] = glm::vec4(
        m[0][3] - m[0][0],
        m[1][3] - m[1][0],
        m[2][3] - m[2][0],
        m[3][3] - m[3][0]
    );

    // Top plane: m[3] - m[1]
    _planes[static_cast<uint32_t>(PlaneIndex::Top)] = glm::vec4(
        m[0][3] - m[0][1],
        m[1][3] - m[1][1],
        m[2][3] - m[2][1],
        m[3][3] - m[3][1]
    );

    // Bottom plane: m[3] + m[1]
    _planes[static_cast<uint32_t>(PlaneIndex::Bottom)] = glm::vec4(
        m[0][3] + m[0][1],
        m[1][3] + m[1][1],
        m[2][3] + m[2][1],
        m[3][3] + m[3][1]
    );

    // Near plane: m[3] + m[2]
    _planes[static_cast<uint32_t>(PlaneIndex::Near)] = glm::vec4(
        m[0][3] + m[0][2],
        m[1][3] + m[1][2],
        m[2][3] + m[2][2],
        m[3][3] + m[3][2]
    );

    // Far plane: m[3] - m[2]
    _planes[static_cast<uint32_t>(PlaneIndex::Far)] = glm::vec4(
        m[0][3] - m[0][2],
        m[1][3] - m[1][2],
        m[2][3] - m[2][2],
        m[3][3] - m[3][2]
    );

    // Normalize all planes
    for (auto& plane : _planes) {
        normalizePlane(plane);
    }
}

bool Frustum::testSphere(const glm::vec3& center, float radius) const {
    // Test sphere against all 6 planes
    // Sphere is outside if it's completely behind any plane
    for (const auto& plane : _planes) {
        const float distance = distanceToPlane(plane, center);
        if (distance < -radius) {
            // Sphere is completely outside this plane
            return false;
        }
    }
    // Sphere is at least partially inside all planes
    return true;
}

bool Frustum::testAABB(const glm::vec3& minPoint, const glm::vec3& maxPoint) const {
    // Test AABB against all 6 planes
    // Use "p-vertex" technique: for each plane, find the corner of the box
    // that is furthest in the direction of the plane normal
    for (const auto& plane : _planes) {
        const glm::vec3 normal(plane.x, plane.y, plane.z);

        // Find the "positive vertex" (furthest point in direction of normal)
        glm::vec3 pVertex;
        pVertex.x = (normal.x >= 0.0f) ? maxPoint.x : minPoint.x;
        pVertex.y = (normal.y >= 0.0f) ? maxPoint.y : minPoint.y;
        pVertex.z = (normal.z >= 0.0f) ? maxPoint.z : minPoint.z;

        // If the positive vertex is outside this plane, the entire box is outside
        if (distanceToPlane(plane, pVertex) < 0.0f) {
            return false;
        }
    }
    // Box is at least partially inside all planes
    return true;
}

bool Frustum::isValid() const {
    // Check if all planes have normalized normals (length ~1.0)
    for (const auto& plane : _planes) {
        const glm::vec3 normal(plane.x, plane.y, plane.z);
        const float length = glm::length(normal);
        if (std::abs(length - 1.0f) > 0.01f) {
            return false;
        }
    }
    return true;
}

void Frustum::normalizePlane(glm::vec4& plane) {
    // Normalize the plane equation so the normal vector is unit length
    const glm::vec3 normal(plane.x, plane.y, plane.z);
    const float length = glm::length(normal);
    if (length > 0.0001f) {
        plane /= length;
    }
}

float Frustum::distanceToPlane(const glm::vec4& plane, const glm::vec3& point) {
    // Calculate signed distance: ax + by + cz + d
    // Positive = in front of plane (inside frustum)
    // Negative = behind plane (outside frustum)
    return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}

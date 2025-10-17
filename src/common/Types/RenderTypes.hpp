#pragma once

#include <glm/glm.hpp>

// Vertex structure for general mesh rendering
struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

// Vertex structure specifically for voxel/chunk rendering
struct VoxelVertex {
    glm::vec3 position; // Local chunk position
    float uv_x;
    glm::vec3 normal; // Face normal
    float uv_y;
    glm::vec4 color; // Block color (temp, will use texture ID later)
};

// Push constants for chunk/voxel rendering
struct ChunkPushConstants {
    glm::mat4 viewProjection;
    glm::vec3 chunkWorldPos;
    float padding; // Align to 16 bytes
};

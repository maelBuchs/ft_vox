#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// Vertex structure for general mesh rendering
struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

// --- PACKED VERTEX DATA ---
// Bit layout: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][Spare:2]
using VoxelVertex = uint32_t;

// Push constants for chunk/voxel rendering
struct ChunkPushConstants {
    glm::mat4 viewProjection;
    glm::vec3 chunkWorldPos;
    float padding; // Align to 16 bytes
};

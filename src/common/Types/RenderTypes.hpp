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

// GPU data for Multi-Draw Indirect rendering (HYBRID approach)
// This will be stored in an SSBO and indexed by gl_DrawID
// MUST match the shader struct in voxel.vert!
struct alignas(16) GPUChunkData {
    glm::vec3 chunkWorldPos;
    uint32_t indexCount;
    uint64_t vertexBufferAddress; // VkDeviceAddress for per-chunk vertex buffer
    uint32_t firstIndex;           // Offset in mega index buffer (needed for GPU culling)
    uint32_t _padding;             // Align to 16 bytes
};

// GPU frustum data for compute culling
// 6 planes (left, right, top, bottom, near, far) + camera position
// MUST match the shader struct in frustum_cull.comp!
struct alignas(16) GPUFrustumData {
    glm::vec4 planes[6];  // Each plane: vec4(a, b, c, d) for ax + by + cz + d = 0
    glm::vec3 cameraPos;  // Camera position for distance calculations
    float maxRenderDistance; // Maximum render distance (for early culling)
};

// Culling statistics (CPU-side readback for debug)
struct CullingStats {
    uint32_t totalChunks;
    uint32_t visibleChunks;
    uint32_t culledChunks;
    float cullingPercentage;
};

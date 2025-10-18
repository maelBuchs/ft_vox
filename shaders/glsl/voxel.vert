#version 460
#extension GL_ARB_shader_draw_parameters : require

// --- PACKED VERTEX INPUT ---
// We now receive a single uint as vertex input
layout(location = 0) in uint inVertexData;

// GLOBAL data - same for all draws in this batch
layout(push_constant) uniform constants {
    mat4 viewProjection;
}
PushConstants;

// PER-CHUNK data - indexed by the draw call
struct GPUChunkData {
    vec3 chunkWorldPos;
    float padding;
};

// SSBO containing per-chunk data
layout(set = 0, binding = 0) readonly buffer ChunkDataBuffer {
    GPUChunkData chunks[];
}
chunkBuffer;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;

// Lookup table for normals, indexed by Normal ID
const vec3 NORMALS[6] = vec3[](vec3(1.0, 0.0, 0.0),  // 0: East
                               vec3(-1.0, 0.0, 0.0), // 1: West
                               vec3(0.0, 1.0, 0.0),  // 2: Top
                               vec3(0.0, -1.0, 0.0), // 3: Bottom
                               vec3(0.0, 0.0, 1.0),  // 4: North
                               vec3(0.0, 0.0, -1.0)  // 5: South
);

// Lookup table for UVs, indexed by UV Corner ID
const vec2 UVS[4] = vec2[](vec2(0.0, 0.0), // 0: Bottom-left
                           vec2(1.0, 0.0), // 1: Bottom-right
                           vec2(1.0, 1.0), // 2: Top-right
                           vec2(0.0, 1.0)  // 3: Top-left
);

void main() {
    // --- UNPACKING LOGIC ---
    // Bit layout: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][Spare:2]
    uint x = (inVertexData) & 0x3Fu;
    uint y = (inVertexData >> 6) & 0x3Fu;
    uint z = (inVertexData >> 12) & 0x3Fu;

    uint normalId = (inVertexData >> 18) & 0x7u;
    uint uvId = (inVertexData >> 21) & 0x3u;
    uint textureId = (inVertexData >> 23) & 0x7Fu;

    vec3 inPosition = vec3(float(x), float(y), float(z));
    vec3 normal = NORMALS[normalId];
    vec2 uv = UVS[uvId];
    // --- END UNPACKING LOGIC ---

    // --- Get per-chunk data from SSBO ---
    // In Vulkan multi-draw indirect, we use gl_InstanceIndex
    // We set firstInstance in the indirect command to identify each chunk
    GPUChunkData chunkData = chunkBuffer.chunks[gl_DrawID];
    vec3 worldPos = inPosition + chunkData.chunkWorldPos;

    gl_Position = PushConstants.viewProjection * vec4(worldPos, 1.0);

    outNormal = normal;
    outUV = uv;

    // Use debug color based on normal (since we don't have texture atlas yet)
    outColor = vec4(abs(normal), 1.0);
}

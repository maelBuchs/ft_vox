#version 460
#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// ============================================================================
// BUFFER DEVICE ADDRESS EXTENSIONS
// ============================================================================
// GL_EXT_buffer_reference allows us to use GPU pointers to access per-chunk buffers.
// Each chunk has its own dedicated vertex/index buffer in VRAM.
// No more mega-buffers, no more fragmentation, instant allocation/deallocation!
// ============================================================================

// Buffer references for per-chunk vertex data access
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    uint vertices[];
};

// GLOBAL data - same for all draws in this batch
layout(push_constant) uniform constants {
    mat4 viewProjection;
}
PushConstants;

// PER-CHUNK data - indexed by the draw call (HYBRID approach)
// NOTE: Buffer reference types must be uint64_t in SSBO
struct GPUChunkData {
    vec3 chunkWorldPos;
    uint indexCount;
    uint64_t vertexBufferAddress;   // Device address of this chunk's vertex buffer
    uint firstIndex;                 // First index in the mega index buffer
    uint _padding;                   // Padding to match C++ struct alignment
    // Note: Index buffer is shared (mega buffer), accessed via traditional binding
};

// SSBO containing per-chunk data
layout(set = 0, binding = 0, std430) readonly buffer ChunkDataBuffer {
    GPUChunkData chunks[];
}
chunkBuffer;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out uint outTextureId;
layout(location = 5) flat out uint outNormalId;
layout(location = 6) out float outAO;

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
    // ========================================================================
    // BINDLESS BUFFER ACCESS - Get this chunk's data via gl_DrawID
    // ========================================================================
    GPUChunkData chunkData = chunkBuffer.chunks[gl_DrawID];

    // Convert device address to buffer reference
    VertexBuffer vertexBuffer = VertexBuffer(chunkData.vertexBufferAddress);

    // ========================================================================
    // FETCH VERTEX DATA from this chunk's dedicated buffer
    // ========================================================================
    uint inVertexData = vertexBuffer.vertices[gl_VertexIndex];

    // Bit layout: [X:6][Y:6][Z:6][Normal:3][UV:2][Texture:7][AO:2]
    uint x = (inVertexData) & 0x3Fu;
    uint y = (inVertexData >> 6) & 0x3Fu;
    uint z = (inVertexData >> 12) & 0x3Fu;

    uint normalId = (inVertexData >> 18) & 0x7u;
    uint uvId = (inVertexData >> 21) & 0x3u;
    uint textureId = (inVertexData >> 23) & 0x7Fu;
    uint ao = (inVertexData >> 30) & 0x3u;

    vec3 inPosition = vec3(float(x), float(y), float(z));
    vec3 normal = NORMALS[normalId];
    vec2 uv = UVS[uvId];

    // Convert AO to brightness multiplier (0=darkest, 3=brightest)
    // Invert: 0 -> 1.0, 1 -> 0.8, 2 -> 0.6, 3 -> 0.4
    outAO = 1.0 - (float(ao) * 0.25);

    // ========================================================================
    // TRANSFORM TO WORLD SPACE and project
    // ========================================================================
    vec3 worldPos = inPosition + chunkData.chunkWorldPos;
    gl_Position = PushConstants.viewProjection * vec4(worldPos, 1.0);

    outNormal = normal;
    outUV = uv;
    outColor = vec4(abs(normal), 1.0);
    outTextureId = textureId;
    outNormalId = normalId;
}

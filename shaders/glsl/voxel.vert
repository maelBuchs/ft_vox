#version 450

// Vertex input
layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inUV_X;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in float inUV_Y;
layout(location = 4) in vec4 inColor;

layout(push_constant) uniform constants {
    mat4 viewProjection;
    vec3 chunkWorldPos;
    float padding;
}
PushConstants;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;

void main() {
    vec3 worldPos = inPosition + PushConstants.chunkWorldPos;

    gl_Position = PushConstants.viewProjection * vec4(worldPos, 1.0);

    outNormal = inNormal;
    outColor = inColor;
    outUV = vec2(inUV_X, inUV_Y);
}

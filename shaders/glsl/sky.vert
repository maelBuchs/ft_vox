#version 460

// Push constants for sky rendering
layout(push_constant) uniform constants {
    mat4 inverseViewProj; // Inverse of (projection * viewWithoutTranslation)
    float timeOfDay;      // 0.0 to 1.0 representing the time of day
    float padding1;
    float padding2;
    float padding3;
}
PushConstants;

// Output to fragment shader
layout(location = 0) out vec3 outViewDir;

void main() {
    // Full-screen triangle technique
    // Generates 3 vertices: (-1,-1), (3,-1), (-1,3) in NDC space
    // This covers the entire screen with a single triangle
    vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

    vec2 pos = positions[gl_VertexIndex];

    // Output position in NDC space (no transformation needed)
    // Depth = 1.0 to place sky at the far plane (furthest from camera)
    gl_Position = vec4(pos, 1.0, 1.0);

    // Calculate view direction for this fragment
    // We reconstruct the view-space ray by transforming from NDC to world space
    vec4 clipPos = vec4(pos, 1.0, 1.0); // z=1.0 for far plane
    vec4 worldPos = PushConstants.inverseViewProj * clipPos;

    // Perspective divide to get the actual world position
    outViewDir = worldPos.xyz / worldPos.w;
}

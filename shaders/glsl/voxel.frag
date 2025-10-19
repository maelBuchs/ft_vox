#version 460

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inTextureId;

layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 1) uniform sampler2D textureAtlas;

// This should match the atlas layout logic in your C++ code.
// Will be dynamically calculated based on actual atlas size
const int TEXTURES_PER_ROW = 3; // Adjust based on actual atlas (3x3 for ~6 textures)
const float TEXTURE_TILE_SIZE = 1.0 / float(TEXTURES_PER_ROW);

void main() {
    // Calculate the UV offset for the correct tile in the atlas
    float tileX = float(inTextureId % uint(TEXTURES_PER_ROW));
    float tileY = float(inTextureId / uint(TEXTURES_PER_ROW));

    vec2 tileOffset = vec2(tileX, tileY) * TEXTURE_TILE_SIZE;

    // Scale the face's local UVs and apply the offset
    vec2 finalUV = tileOffset + (inUV * TEXTURE_TILE_SIZE);

    // Sample the color from the texture atlas
    vec4 textureColor = texture(textureAtlas, finalUV);

    // Basic lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(inNormal, lightDir), 0.0);

    float ambient = 0.6;
    float diffuse = 0.4 * diff;
    float lighting = ambient + diffuse;

    outFragColor = vec4(textureColor.rgb * lighting, textureColor.a);

    // Note: Transparent fragment discarding disabled to avoid requiring
    // shaderDemoteToHelperInvocation Vulkan 1.3 feature
    // if (outFragColor.a < 0.1) {
    //     discard;
    // }
}

#version 460

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inTextureId;
layout(location = 4) flat in uint inDummy; // Dummy to maintain contiguous locations
layout(location = 5) flat in uint inNormalId;
layout(location = 6) in float inAO;

layout(location = 0) out vec4 outFragColor;

layout(set = 1, binding = 0) uniform sampler2D textureAtlas;

// Atlas configuration passed from C++
layout(set = 1, binding = 1) uniform AtlasConfig {
    int texturesPerRow;
}
atlasConfig;

layout(push_constant) uniform constants {
    mat4 viewProj;
    uint needsGammaCorrection;
}
PushConstants;

// Hardcoded ID for grass top texture (must match load order in C++)
const uint GRASS_TOP_TEXTURE_ID = 2u;

// Normal ID constants
const uint EAST_ID = 0u;
const uint WEST_ID = 1u;
const uint TOP_ID = 2u;
const uint BOTTOM_ID = 3u;
const uint NORTH_ID = 4u;
const uint SOUTH_ID = 5u;

void main() {
    // Calculate the UV offset for the correct tile in the atlas
    // Use dynamic value from uniform instead of hardcoded constant
    uint texturesPerRow = uint(atlasConfig.texturesPerRow);
    float tileSizeInAtlas = 1.0 / float(texturesPerRow);

    float tileX = float(inTextureId % texturesPerRow);
    float tileY = float(inTextureId / texturesPerRow);

    vec2 tileOffset = vec2(tileX, tileY) * tileSizeInAtlas;

    vec2 tiledUV = fract(inUV);

    // Scale the face's local UVs and apply the offset
    vec2 finalUV = tileOffset + (tiledUV * tileSizeInAtlas);

    // Sample the color from the texture atlas
    vec4 textureColor = texture(textureAtlas, finalUV);

    // Biome Tinting Logic
    vec3 finalColor = textureColor.rgb;
    if (inTextureId == GRASS_TOP_TEXTURE_ID) {
        // Lush green color for default biome
        vec3 grassBiomeColor = vec3(0.34, 0.65, 0.23);
        finalColor = textureColor.rgb * grassBiomeColor;
    }

    // Directional Shading
    float faceBrightness = 1.0;
    if (inNormalId == TOP_ID) {
        faceBrightness = 1.0; // Top faces are fully lit
    } else if (inNormalId == EAST_ID || inNormalId == WEST_ID || inNormalId == NORTH_ID ||
               inNormalId == SOUTH_ID) {
        faceBrightness = 0.8; // Side faces are slightly darker
    } else if (inNormalId == BOTTOM_ID) {
        faceBrightness = 0.6; // Bottom faces are darkest
    }

    // Basic lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(inNormal, lightDir), 0.0);

    float ambient = 0.6;
    float diffuse = 0.4 * diff;
    float lighting = ambient + diffuse;

    // Apply lighting, directional brightness, and ambient occlusion
    vec3 finalOutputColor = finalColor * lighting * faceBrightness * inAO;
    vec3 encoded = (PushConstants.needsGammaCorrection == 1u)
                       ? pow(finalOutputColor, vec3(1.0 / 2.2))
                       : finalOutputColor;
    outFragColor = vec4(encoded, textureColor.a);

    // Note: Transparent fragment discarding disabled to avoid requiring
    // shaderDemoteToHelperInvocation Vulkan 1.3 feature
    // if (outFragColor.a < 0.1) {
    //     discard;
    // }
}

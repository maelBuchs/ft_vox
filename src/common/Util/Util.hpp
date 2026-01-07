#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "Spline.hpp"

struct NoiseParams {
    float baseFrequency;
    int octaves;
    float persistence;
};

struct BiomeParams {
    float kTEMPERATURE;
    float humidity;
    float continent;
    float erosion;
    float weirdness;
    float depth;
};
namespace noise_config {
constexpr NoiseParams kTEMPERATURE{.baseFrequency = 0.005F, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams kHUMIDITY{.baseFrequency = 0.005F, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams kCONTINENT{.baseFrequency = 0.001F, .octaves = 8, .persistence = 0.5F};
constexpr NoiseParams kEROSION{.baseFrequency = 0.02F, .octaves = 3, .persistence = 0.5F};
constexpr NoiseParams kWEIRDNESS{.baseFrequency = 0.03F, .octaves = 2, .persistence = 0.5F};
constexpr NoiseParams kDEPTH{.baseFrequency = 0.04F, .octaves = 1, .persistence = 0.5F};
constexpr NoiseParams kBASE_ELEVATION{.baseFrequency = 0.01F, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams kCAVE{.baseFrequency = 0.018F, .octaves = 2, .persistence = 0.5F};
} // namespace noise_config

float perlinValue(float x, float y, int64_t seed);
float perlinNoise(int x, int y, NoiseParams params, int64_t seed);
float perlinValue3D(float x, float y, float z, int64_t seed);
float perlinNoise3D(int x, int y, int z, NoiseParams params, int64_t seed);

uint32_t quadraticCongruential(int x, int y, uint32_t seed);

// On garde (int b, int c) comme dans ton code original
constexpr int chunkToWorld(int b, int c) {
    // Utiliser + au lieu de | est plus sûr si b est hors limites (ex: 32)
    return (c << 5) + b;
}

constexpr int worldToChunk(int w) {
    // Le décalage de bits (arithmétique) est l'équivalent parfait de floor(w/32.0)
    // Fonctionne pour -1 (donne -1) et -32 (donne -1) et -33 (donne -2)
    return w >> 5;
}

constexpr int worldToBlock(int w) {
    // Le masque & 31 remplace avantageusement le modulo pour les nombres négatifs
    return w & 31;
}
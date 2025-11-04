#pragma once

#include <vector>

#include <glm/glm.hpp>

struct NoiseParams {
    float baseFrequency;
    int64_t seed;
    int octaves;
    float persistence;
};
#ifndef SEED

constexpr int64_t SEED = 42L;
#endif
struct BiomeParams {
    float temperature;
    float humidity;
    float continent;
    float erosion;
    float weirdness;
    float depth;
};
namespace NoiseConfig {
constexpr NoiseParams TEMPERATURE{
    .baseFrequency = 0.01F, .seed = SEED, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams HUMIDITY{
    .baseFrequency = 0.01F, .seed = SEED, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams CONTINENT{
    .baseFrequency = 0.005F, .seed = SEED, .octaves = 5, .persistence = 0.5F};
constexpr NoiseParams EROSION{
    .baseFrequency = 0.02F, .seed = SEED, .octaves = 3, .persistence = 0.5F};
constexpr NoiseParams WEIRDNESS{
    .baseFrequency = 0.03F, .seed = SEED, .octaves = 2, .persistence = 0.5F};
constexpr NoiseParams DEPTH{
    .baseFrequency = 0.04F, .seed = SEED, .octaves = 1, .persistence = 0.5F};
} // namespace NoiseConfig

float perlinValue(float x, float y, int64_t seed);
float perlinNoise(int x, int y, NoiseParams params);

uint32_t quadraticCongruential(int x, int y, uint32_t seed);
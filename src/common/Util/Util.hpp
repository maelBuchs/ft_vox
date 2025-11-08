#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "Spline.hpp"

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
    float kTEMPERATURE;
    float humidity;
    float continent;
    float erosion;
    float weirdness;
    float depth;
};
namespace noise_config {
constexpr NoiseParams kTEMPERATURE{
    .baseFrequency = 0.005F, .seed = SEED, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams kHUMIDITY{
    .baseFrequency = 0.005F, .seed = SEED, .octaves = 4, .persistence = 0.5F};
constexpr NoiseParams kCONTINENT{
    .baseFrequency = 0.001F, .seed = SEED, .octaves = 7, .persistence = 0.5F};
constexpr NoiseParams kEROSION{
    .baseFrequency = 0.02F, .seed = SEED, .octaves = 3, .persistence = 0.5F};
constexpr NoiseParams kWEIRDNESS{
    .baseFrequency = 0.03F, .seed = SEED, .octaves = 2, .persistence = 0.5F};
constexpr NoiseParams kDEPTH{
    .baseFrequency = 0.04F, .seed = SEED, .octaves = 1, .persistence = 0.5F};
constexpr NoiseParams kBASE_ELEVATION{
    .baseFrequency = 0.01F, .seed = SEED, .octaves = 4, .persistence = 0.5F};
} // namespace noise_config

float perlinValue(float x, float y, int64_t seed);
float perlinNoise(int x, int y, NoiseParams params);

uint32_t quadraticCongruential(int x, int y, uint32_t seed);
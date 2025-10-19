#pragma once

#include <vector>

#include <glm/glm.hpp>
std::vector<std::vector<float>> perlinNoise(int width, int height, float baseFrequency,
                                            long int seed, int octaves, float persistence);
float perlinNoiseByCoordinates(float x, float y, float baseFrequency, long int seed, int octaves,
                               float persistence);
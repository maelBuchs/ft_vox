#include "perlinNoise.hpp"

#include <cmath>
#include <numbers>

namespace {
// Génère un vecteur gradient pseudo-aléatoire basé sur les coordonnées
glm::vec2 randomGradient(int ix, int iy, unsigned int seed) {
    unsigned int h = (ix * 374761393) + (iy * 668265263);
    h ^= (seed * 0x27d4eb2d); // multiplier la seed pour la disperser
    h = (h ^ (h >> 13)) * 1274126177;
    float angle = static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU) * 2.0F *
                  std::numbers::pi_v<float>;
    return glm::vec2(std::cos(angle), std::sin(angle));
}
// Produit le produit scalaire distance * gradient
float dotGridGradient(int ix, int iy, float x, float y, long int seed) {
    glm::vec2 gradient = randomGradient(ix, iy, seed);
    float dx = x - (float)ix;
    float dy = y - (float)iy;
    return (dx * gradient[0]) + (dy * gradient[1]);
}

// Fonction fade pour interpolation
float fade(float t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
}

// Interpolation lissée
float interpolate(float a0, float a1, float w) {
    return a0 + (fade(w) * (a1 - a0));
}

// Valeur de Perlin à une position
// Peut etre move out of namespace si besoin
float perlinValue(float x, float y, long int seed) {
    int x0 = static_cast<int>(floorf(x));
    int x1 = x0 + 1;
    int y0 = static_cast<int>(floorf(y));
    int y1 = y0 + 1;

    float sx = x - (float)x0;
    float sy = y - (float)y0;

    float n0 = dotGridGradient(x0, y0, x, y, seed);
    float n1 = dotGridGradient(x1, y0, x, y, seed);
    float ix0 = interpolate(n0, n1, sx);

    n0 = dotGridGradient(x0, y1, x, y, seed);
    n1 = dotGridGradient(x1, y1, x, y, seed);
    float ix1 = interpolate(n0, n1, sx);

    float value = interpolate(ix0, ix1, sy);
    return value;
}
} // namespace

// Génère une matrice 2D de Perlin noise
// octave : entre 1 et 10
// persistence : entre 0 et 1
std::vector<std::vector<float>> perlinNoise(int width, int height, float baseFrequency,
                                            long int seed, int octaves, float persistence) {
    std::vector<std::vector<float>> perlin(height, std::vector<float>(width));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float amplitude = 1.0F;
            float frequency = baseFrequency;
            float noiseValue = 0.0F;
            float maxValue = 0.0F; // Pour normalisation

            for (int o = 0; o < octaves; ++o) {
                noiseValue += perlinValue(static_cast<float>(x) * frequency,
                                          static_cast<float>(y) * frequency, seed) *
                              amplitude;
                maxValue += amplitude;
                amplitude *= persistence;
                frequency *= 2.0F;
            }

            perlin[y][x] = noiseValue / maxValue; // Normalisation à [-1,1] approximatif
        }
    }
    return perlin;
}
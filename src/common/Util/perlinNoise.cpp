#include <cmath>
#include <cstddef>
#include <numbers>
#include <sys/stat.h>

#include "Util.hpp"

namespace {
// Génère un vecteur gradient pseudo-aléatoire basé sur les coordonnées
glm::vec2 randomGradient(int ix, int iy, int64_t seed) {
    unsigned int h = (ix * 374761393) + (iy * 668265263);
    h ^= (seed * 0x27d4eb2d); // multiplier la seed pour la disperser
    h = (h ^ (h >> 13)) * 1274126177;
    float angle = static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU) * 2.0F *
                  std::numbers::pi_v<float>;
    return glm::vec2(std::cos(angle), std::sin(angle));
}
// Produit le produit scalaire distance * gradient
float dotGridGradient(int ix, int iy, float x, float y, int64_t seed) {
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

} // namespace
// Valeur de Perlin à une position
// Peut etre move out of namespace si besoin
float perlinValue(float x, float y, int64_t seed) {
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
    value = (value + 1.0f) * 0.5f;

    return value;
}

// Génère une matrice 2D de Perlin noise
// octave : entre 1 et 10
// persistence : entre 0 et 1
float perlinNoise(int x, int y, NoiseParams params) {

    float total = 0.0F;
    float frequency = params.baseFrequency;
    float amplitude = 1.0F;
    float maxValue = 0.0F; // Pour normaliser le résultat

    for (int i = 0; i < params.octaves; ++i) {
        total += perlinValue(static_cast<float>(x) * frequency, static_cast<float>(y) * frequency,
                             params.seed + (static_cast<int64_t>(i) * 100)) *
                 amplitude;

        maxValue += amplitude;

        amplitude *= params.persistence;
        frequency *= 2.0F;
    }
    maxValue == 0 ? 0 : total / maxValue;
    float normalized = maxValue == 0 ? 0 : total / maxValue;
    // Remap de [0,1] à [-2,2]
    return (normalized * 4.0f) - 2.0f;
}
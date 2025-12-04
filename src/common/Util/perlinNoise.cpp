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

// Hash function rapide pour 3 coordonnées
unsigned int hash3D(int ix, int iy, int iz, int64_t seed) {
    // Ajout d'un grand nombre premier pour Z
    unsigned int h = (ix * 374761393) + (iy * 668265263) + (iz * 287313881);
    h ^= (seed * 0x27d4eb2d);
    h = (h ^ (h >> 13)) * 1274126177;
    return h;
}

// Calcul du produit scalaire en 3D sans créer de vec3 (Optimisation standard Perlin)
// Au lieu de générer un angle aléatoire, on choisit parmi 12 directions prédéfinies
float dotGridGradient3D(int ix, int iy, int iz, float x, float y, float z, int64_t seed) {
    // 1. Calcul du hash
    unsigned int h = hash3D(ix, iy, iz, seed);

    // 2. Vecteurs de distance
    float dx = x - (float)ix;
    float dy = y - (float)iy;
    float dz = z - (float)iz;

    // 3. "Gradient hashing" (Ken Perlin Improved Noise)
    // Utilise les bits du hash pour choisir un vecteur (1,1,0), (-1,1,0), etc.
    int hash = h & 15;

    // Si hash < 8 on prend x, sinon y
    float u = hash < 8 ? dx : dy;

    // Si hash < 4 on prend y, sinon si 12 ou 14 on prend x, sinon z
    float v = hash < 4 ? dy : (hash == 12 || hash == 14 ? dx : dz);

    // Calcul du signe basé sur les bits 1 et 2
    return ((hash & 1) == 0 ? u : -u) + ((hash & 2) == 0 ? v : -v);
}

} // namespace

// 2D NOISE

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
float perlinNoise(int x, int y, NoiseParams params, int64_t seed) {

    float total = 0.0F;
    float frequency = params.baseFrequency;
    float amplitude = 1.0F;
    float maxValue = 0.0F; // Pour normaliser le résulta

    for (int i = 0; i < params.octaves; ++i) {
        total += perlinValue(static_cast<float>(x) * frequency, static_cast<float>(y) * frequency,
                             seed + (static_cast<int64_t>(i) * 100)) *
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

// 3D NOISE

float perlinValue3D(float x, float y, float z, int64_t seed) {
    // Cube unitaire
    int x0 = static_cast<int>(floorf(x));
    int x1 = x0 + 1;
    int y0 = static_cast<int>(floorf(y));
    int y1 = y0 + 1;
    int z0 = static_cast<int>(floorf(z));
    int z1 = z0 + 1;

    // Position relative
    float sx = x - (float)x0;
    float sy = y - (float)y0;
    float sz = z - (float)z0;

    // Interpolation sur X (4 interpolations)
    // Face Avant (Z0)
    float n0 = dotGridGradient3D(x0, y0, z0, x, y, z, seed);
    float n1 = dotGridGradient3D(x1, y0, z0, x, y, z, seed);
    float ix0 = interpolate(n0, n1, sx);

    float n2 = dotGridGradient3D(x0, y1, z0, x, y, z, seed);
    float n3 = dotGridGradient3D(x1, y1, z0, x, y, z, seed);
    float ix1 = interpolate(n2, n3, sx);

    // Face Arrière (Z1)
    float n4 = dotGridGradient3D(x0, y0, z1, x, y, z, seed);
    float n5 = dotGridGradient3D(x1, y0, z1, x, y, z, seed);
    float ix2 = interpolate(n4, n5, sx);

    float n6 = dotGridGradient3D(x0, y1, z1, x, y, z, seed);
    float n7 = dotGridGradient3D(x1, y1, z1, x, y, z, seed);
    float ix3 = interpolate(n6, n7, sx);

    // Interpolation sur Y (2 interpolations)
    float iy0 = interpolate(ix0, ix1, sy); // Z0
    float iy1 = interpolate(ix2, ix3, sy); // Z1

    // Interpolation sur Z (1 interpolation finale)
    float value = interpolate(iy0, iy1, sz);

    // Normalisation approximative vers [0, 1]
    return (value + 1.0f) * 0.5f;
}

float perlinNoise3D(int x, int y, int z, NoiseParams params, int64_t seed) {
    float total = 0.0F;
    float frequency = params.baseFrequency;
    float amplitude = 1.0F;
    float maxValue = 0.0F;

    for (int i = 0; i < params.octaves; ++i) {
        // Décalage de la seed par octave pour éviter que les couches s'alignent
        int64_t octaveSeed = seed + (static_cast<int64_t>(i) * 100);

        total += perlinValue3D(static_cast<float>(x) * frequency, static_cast<float>(y) * frequency,
                               static_cast<float>(z) * frequency, // Ajout du Z
                               octaveSeed) *
                 amplitude;

        maxValue += amplitude;

        amplitude *= params.persistence;
        frequency *= 2.0F;
    }

    float normalized = maxValue == 0 ? 0 : total / maxValue;

    // Remap de [0,1] à [-2,2] comme dans ta fonction 2D
    return (normalized * 4.0f) - 2.0f;
}
#include <cmath>
#include <cstdlib>

#include <tracy/Tracy.hpp>

#include "common/World/Chunk.hpp"
#include "WorldManager.hpp"
namespace {

int getHeightValue(int bx, int bz, float continentalness, BiomeType biome,
                   tk::spline& heightSpline) {

    float baseHeight = static_cast<float>(heightSpline(continentalness * 2.0F));
    return baseHeight;
    // --- 1. Bruits de base ---
    // float erosion = perlinNoise(bx, bz, noise_config::kEROSION);

    // continentalness = std::clamp(continentalness, -1.0F, 1.0F);
    // erosion = (erosion + 1.0F) * 0.5F; // [-1,1] -> [0,1]

    // float y = 0.0F;

    // if (continentalness < 0.0F || biome == BiomeType::kOCEAN) {
    //     float oceanDepth = (-continentalness) * 40.0F;
    //     y = -oceanDepth + (heightNoise * 10.0F); // petites vagues
    // } else {
    //     float baseHeight = continentalness * continentalness;
    //     float mountainFactor = 1.0F - erosion;
    //     y = baseHeight * (0.5F + mountainFactor * 0.8F);

    //     if (continentalness > 0.6F && erosion < 0.3F) {
    //         y += (0.6F - erosion) * 0.8F;
    //     }

    //     // Échelle finale (hauteur max ~ 280)
    //     y = (y * 280.0F) + (heightNoise * 15.0F);
    // }
    // return static_cast<int>(y);
}

// int getHeightValue(int bx, int bz, float continentalness, BiomeType biome) {
//     // Clamp la continentalness dans [-1, 1]
//     float heightNoise = perlinNoise(bx, bz, noise_config::kBASE_ELEVATION);
//     continentalness = std::clamp(continentalness, -1.0F, 1.0F);
//     float erosion = perlinNoise(bx, bz, noise_config::kEROSION);
//     // // Remappage de la continentalness vers la hauteur brute
//     float y = 0.0F;
//     y = (continentalness + 1.0F) / 2.0F * 200.0F;
//     if (biome == BiomeType::kOCEAN) {
//         return static_cast<int>(y * 0.3f + heightNoise * 20.0F);
//     }
//     // if (biome != BiomeType::kOCEAN)
//     //     y *= abs(erosion) + 0.5f;
//     continentalness *= continentalness;
//     float mountainFactor = 1.0F - erosion;
//     y = continentalness * (0.5f + mountainFactor);
//     return static_cast<int>(y * 280.0F + heightNoise * 15.0F);
//     // if (BiomeType::kOCEAN == biome) {
//     //     // Océan profond
//     //     y = 5.0F + (1 + 1.0F) / 0.2f * (20.0F - 5.0F);
//     //     // } else if (continentalness <= -0.4f) {
//     //     // Océan peu profond / côte
//     //     // y = 20.0F + (continentalness + 0.8f) / 0.4f * (60.0F - 20.0F);
//     // } else if (biome == BiomeType::kPLAINS) {
//     //     // Terre continentale
//     //     y = 50.0F + (1 + 0.4f) / 0.8f * (100.0F - 50.0F);
//     // } else if (biome == BiomeType::kMOUNTAINS) {
//     //     // Montagne
//     //     y = 100.0F + (1 - 0.4f) / 0.6f * (180.0F - 100.0F);
//     // }

//     // // === Modulation par biome ===
//     // switch (biome) {
//     // case BiomeType::kPLAINS:
//     //     y *= 1.0F;
//     //     break;
//     // case BiomeType::kMOUNTAINS:
//     //     y *= 1.3f; // plus élevé
//     //     break;
//     // case BiomeType::kOCEAN:
//     //     y *= 0.1f; // relief légèrement accentué
//     // // break;
//     // default:
//     //     break;
//     // }

//     // // === Ajout d’un bruit local pour éviter un relief trop lisse ===
//     // const float frequency = 0.05f;
//     // float baseNoise = perlinValue(bx * frequency, bz * frequency, SEED);
//     // y += baseNoise * 10.0F; // ±10 blocs de variation locale

//     // // Clamp final
//     // y = std::clamp(y, 10.0F, 280.0F);

//     return static_cast<int>(y);
// }

// // int getHeightValue(int bx, int bz, BiomeType biome) {
// //     return 0;
// //     float bxF = static_cast<float>(bx);
// //     float bzF = static_cast<float>(bz);

// //     const float frequency = 0.02F;

// //     float noiseValue = perlinValue(bxF * frequency, bzF * frequency * 0.1, SEED);
// //     noiseValue += 0.5F * perlinValue(bxF * frequency * 2.0F, bzF * frequency * 2.0F,
// SEED);
// //     noiseValue += 0.25F * perlinValue(bxF * frequency * 4.0F, bzF * frequency * 4.0F,
// SEED);
// //     noiseValue /= 1.0F + 0.5F + 0.25F;
// //     const float min_height = 0.0F;
// //     const float max_height = 280.0F;
// //     noiseValue = std::pow(noiseValue, 0.8F);
// //     // int height = static_cast<int>(min_height + noiseValue * (max_height - min_height));
// //     // int height = static_cast<int>(noiseValue);
// //     int height = static_cast<int>(min_height + (noiseValue * (max_height - min_height)));

// //     return height;
// // }

void addSurface(Chunk* chunk, const int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE]) {
    // Simple surface addition: set the top block to a different ID (e.g., grass)
    // Use pre-computed heightMap to avoid redundant getHeightValue() calls
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {

            int maxHeight = heightMap[bx][bz];

            if (maxHeight >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, maxHeight % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 1 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 1 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 1) % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 2 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 2 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                chunk->setBlock(bx, (maxHeight + 2) % Chunk::CHUNK_SIZE, bz, 3);
            }
            if (maxHeight + 3 >= CHUNK_TO_WORLD(0, chunk->getPosition()[1]) &&
                maxHeight + 3 < CHUNK_TO_WORLD(Chunk::CHUNK_SIZE, chunk->getPosition()[1])) {
                auto biome = chunk->getBiomeDataAt(bx, bz);
                if (biome == BiomeType::kPLAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 2);
                } else if (biome == BiomeType::kMOUNTAINS) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 1);
                } else if (biome == BiomeType::kOCEAN) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 3);
                } else if (biome == BiomeType::kNONE) {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 4);
                } else {
                    chunk->setBlock(bx, (maxHeight + 3) % Chunk::CHUNK_SIZE, bz, 5);
                }
            }

            if (chunk->getPosition()[1] == 0) {
                chunk->setBlock(bx, 0, bz, 5);
            }
        }
    }
}
} // namespace
std::shared_ptr<Chunk> WorldManager::generateChunk(const glm::ivec3& pos) {
    ZoneScoped;

    auto chunk = std::make_shared<Chunk>(pos[0], pos[1], pos[2]);

    // Pre-compute height map once to avoid redundant calculations
    // This cuts height calculations in half (was called in both generateChunk and addSurface)
    int heightMap[Chunk::CHUNK_SIZE][Chunk::CHUNK_SIZE];

    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            heightMap[bx][bz] =
                getHeightValue(CHUNK_TO_WORLD(bx, pos[0]), CHUNK_TO_WORLD(bz, pos[2]),
                               chunk->getNoise(bx, bz, NoiseType::kCONTINENT),
                               chunk->getBiomeDataAt(bx, bz), getHeightSpline());
        }
    }

    // Fill chunk with terrain using pre-computed height map
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            int maxHeight = heightMap[bx][bz];
            for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, pos[1]) < maxHeight) {
                    chunk->setBlock(bx, by, bz, 1); // Set block ID to 1 (solid block)
                }
            }
        }
    }
    for (int bx = 0; bx < Chunk::CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < Chunk::CHUNK_SIZE; bz++) {
            // int maxHeight = heightMap[bx][bz];
            for (int by = 0; by < Chunk::CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, pos[1]) < 80 && chunk->getBlock(bx, by, bz) == 0) {
                    // chunk->setBlock(bx, by, bz, 6); // Set block ID to 1 (solid block)
                }
            }
        }
    }

    // Add surface layers using same height map
    addSurface(chunk.get(), heightMap);
    return chunk;
}

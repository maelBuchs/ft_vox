
#include <cstdint>
#include <iostream>

#include "Util.hpp"

int32_t mix(int64_t n) {
    uint64_t u = static_cast<uint64_t>(n);
    // mélange un peu les bits du signe dans le reste
    u ^= (u >> 30);
    u *= 0xBF58476D1CE4E5B9ULL; // constante arbitraire (tirée de splitmix64)
    u ^= (u >> 27);
    u *= 0x94D049BB133111EBULL;
    u ^= (u >> 31);
    return static_cast<uint32_t>(u ^ (u >> 32));
}

// int32_t quadraticCongruential(int32_t seed, int x, int y, int mod) {
//     int a = mix(x);
//     int b = 1013904223;
//     int c = mix(y);
//     int32_t result = (seed);
//     result = (a * result * result + b * result + c) % mod;
//     // std::cout << "x - y = " << x << " - " << y << std::endl;
//     return result;
// }

uint32_t quadraticCongruential(int x, int y, uint32_t seed) {
    uint32_t h = seed + 1;

    const uint32_t PRIME1 = 198491317;
    const uint32_t PRIME2 = 6542989;

    h ^= static_cast<uint32_t>(x) * PRIME1;
    h ^= static_cast<uint32_t>(y) * PRIME2;

    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}
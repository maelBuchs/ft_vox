#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

struct VoxelVertex {
    glm::vec3 position; // Local chunk position
    float uv_x;
    glm::vec3 normal; // Face normal
    float uv_y;
    glm::vec4 color; // Block color (temp, will use texture ID later)
};

class Chunk {
  public:
    static constexpr int CHUNK_SIZE = 32;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    static constexpr uint8_t AIR_BLOCK_ID = 0;

    Chunk();
    ~Chunk() = default;

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = default;
    Chunk& operator=(Chunk&&) = default;

    // Block access (using block IDs from BlockRegistry)
    [[nodiscard]] uint8_t getBlock(int x, int y, int z) const;
    void setBlock(int x, int y, int z, uint8_t blockId);

    // Helper functions
    [[nodiscard]] bool isBlockSolid(int x, int y, int z) const;
    [[nodiscard]] bool isInBounds(int x, int y, int z) const;
    [[nodiscard]] int getIndex(int x, int y, int z) const;

    // Chunk state
    [[nodiscard]] bool isEmpty() const { return _isEmpty; }
    void setEmpty(bool empty) { _isEmpty = empty; }

  private:
    std::array<uint8_t, VOLUME> _blocks;
    bool _isEmpty = true;
};

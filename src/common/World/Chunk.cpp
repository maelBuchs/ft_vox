#include "Chunk.hpp"

#include "glm/fwd.hpp"

#define CHUNK_TO_WORLD(b, c) ((c) * Chunk::CHUNK_SIZE + (b))
#define SEED 42L

float getHeightValue(int bx, int bz) {
    float noiseValue = perlinNoiseByCoordinates(bx, bz, 0.01F, SEED, 1, 0.1F);

    int min_height = -50;
    int max_height = 150;
    return (min_height + noiseValue * (max_height - min_height));
}

void Chunk::generateChunk() {

    for (int bx = 0; bx < CHUNK_SIZE; bx++) {
        for (int bz = 0; bz < CHUNK_SIZE; bz++) {
            float noiseValue =
                perlinNoiseByCoordinates(CHUNK_TO_WORLD(bx, position.x),
                                         CHUNK_TO_WORLD(bz, position.z), 0.01F, SEED, 1, 0.1F);

            int maxHeight =
                getHeightValue(CHUNK_TO_WORLD(bx, position.x), CHUNK_TO_WORLD(bz, position.z));
            for (int by = 0; by < CHUNK_SIZE; by++) {
                if (CHUNK_TO_WORLD(by, position.y) < maxHeight) {
                    setBlock(bx, by, bz, 2); // Set block ID to 1 (solid block)
                }
            }
        }
    }
}

bool Chunk::loadChunk() {
    // Placeholder for chunk loading logic
    // Return false to indicate chunk does not exist and needs generation
    return false;
}

Chunk::Chunk(int x, int y, int z) : _blocks{}, position{x, y, z} {
    _blocks.fill(AIR_BLOCK_ID);
    if (!loadChunk()) {
        generateChunk();
    }
}

uint8_t Chunk::getBlock(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return AIR_BLOCK_ID;
    }
    return _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z)));
}

void Chunk::setBlock(int x, int y, int z, uint8_t blockId) {
    if (!isInBounds(x, y, z)) {
        return;
    }
    _blocks.at(static_cast<decltype(_blocks)::size_type>(getIndex(x, y, z))) = blockId;
    if (blockId != AIR_BLOCK_ID) {
        _isEmpty = false;
    }
}

bool Chunk::isBlockSolid(int x, int y, int z) const {
    if (!isInBounds(x, y, z)) {
        return false;
    }
    return getBlock(x, y, z) != AIR_BLOCK_ID;
}

bool Chunk::isInBounds(int x, int y, int z) const {
    return x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
}

int Chunk::getIndex(int x, int y, int z) const {
    return x + (y * CHUNK_SIZE) + (z * CHUNK_SIZE * CHUNK_SIZE);
}

// Chunk generation worker thread
/*
  Runs in a separate thread, continuously checking for chunks to generate.
  When a chunk coordinate is available in the _chunksToGenerate queue,
  it generates the chunk and adds it to the _readyChunks queue.
*/
void ChunkInstanciator::startWorker() {
    // Start chunk generation worker thread
    while (running) {
        ChunkCoord coord;
        {
            std::unique_lock lock(_queueMutex);
            if (_chunksToGenerate.empty()) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            coord = _chunksToGenerate.back();
            _chunksToGenerate.pop_back();
        }

        Chunk chunk = Chunk(coord[0], coord[1], coord[2]);

        {
            std::lock_guard lock(_queueMutex);
            _readyChunks.push_back(std::move(chunk));
        }
    }
}

// static bool isInSphere(int x, int y, int z, int radius) {
//     return (x * x + y * y + z * z) <= (radius * radius);
// }

bool ChunkInstanciator::isLoadedOrRequested(int x, int y, int z) {
    std::lock_guard lock(_queueMutex);
    for (const auto& coord : _chunksToGenerate) {
        if (coord[0] == x && coord[1] == y && coord[2] == z) {
            return true;
        }
    }
    for (const auto& chunk : _loadedChunks) {
        glm::ivec3 pos = chunk.getPosition();
        if (pos.x == x && pos.y == y && pos.z == z) {
            return true;
        }
    }
    return false;
}

void ChunkInstanciator::updateChunksAroundPlayer(glm::ivec3 cameraBlockPos) {

    glm::ivec3 playerChunkPos = {cameraBlockPos.x / Chunk::CHUNK_SIZE,
                                 cameraBlockPos.y / Chunk::CHUNK_SIZE,
                                 cameraBlockPos.z / Chunk::CHUNK_SIZE};
    int radius = RENDER_DISTANCE;
    for (int x = -radius; x <= radius; ++x) {
        for (int y = -radius; y <= radius; ++y) {
            for (int z = -radius; z <= radius; ++z) {
                // if (isInSphere(x, y, z, radius) &&
                if (!isLoadedOrRequested(playerChunkPos[0] + x, playerChunkPos[1] + y,
                                         playerChunkPos[2] + z)) {
                    ChunkCoord coord = {playerChunkPos[0] + x, playerChunkPos[1] + y,
                                        playerChunkPos[2] + z};
                    {
                        std::lock_guard lock(_queueMutex);
                        _chunksToGenerate.push_back(coord);
                    }
                }
            }
        }
    }
    unloadChunkIfNeeded(playerChunkPos);
    // std::cout << "Nombre de chunks à générer : " << _chunksToGenerate.size() << std::endl;
}

void ChunkInstanciator::unloadChunkIfNeeded(glm::ivec3 playerChunkPos) {
    for (auto it = _loadedChunks.begin(); it != _loadedChunks.end(); it++) {
        glm::ivec3 pos = it->getPosition();
        if (glm::abs(pos.x - playerChunkPos.x) > RENDER_DISTANCE ||
            glm::abs(pos.y - playerChunkPos.y) > RENDER_DISTANCE ||
            glm::abs(pos.z - playerChunkPos.z) > RENDER_DISTANCE) {
            std::cout << "Unloading chunk at (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
            it = _loadedChunks.erase(it);
            if (it == _loadedChunks.end()) {
                break;
            }
        }
    }
}

// std::cout << "Nombre de chunks à générer : " << _chunksToGenerate.size() << std::endl;
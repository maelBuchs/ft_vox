#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <tracy/Tracy.hpp>
#include <vector>

#include "common/Protocol/Protocol.hpp"
#include "common/Util/ThreadSafeQueue.hpp"
#include "common/World/BlockRegistry.hpp"

/**
 * MeshingThread processes GenerationTask messages and produces MeshData.
 * Multiple instances can run in parallel for efficient CPU utilization.
 */
class MeshingThread {
  public:
    using TextureIdResolver = std::function<uint32_t(const std::string&)>;

    MeshingThread(ThreadSafeQueue<GenerationTask>& taskQueue, ThreadSafeQueue<MeshData>& meshQueue,
                  ThreadSafeQueue<MeshingComplete>& completionQueue,
                  const BlockRegistry& blockRegistry, const TextureIdResolver& textureResolver);
    ~MeshingThread();

    MeshingThread(const MeshingThread&) = delete;
    MeshingThread& operator=(const MeshingThread&) = delete;
    MeshingThread(MeshingThread&&) = delete;
    MeshingThread& operator=(MeshingThread&&) = delete;

    /**
     * Start the meshing thread. Non-blocking.
     */
    void start();

    /**
     * Stop the meshing thread. Blocks until thread exits.
     */
    void stop();

  private:
    /**
     * Meshing thread main loop.
     * Processes GenerationTask and generates mesh data.
     */
    void meshingThreadLoop();

    // Communication queues
    ThreadSafeQueue<GenerationTask>& _taskQueue;
    ThreadSafeQueue<MeshData>& _meshQueue;
    ThreadSafeQueue<MeshingComplete>& _completionQueue;

    // Block data (reference to shared registry)
    const BlockRegistry& _blockRegistry;

    // Texture ID resolver (captures renderer's texture atlas)
    TextureIdResolver _textureResolver;

    // Thread control
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _meshingThread;
};

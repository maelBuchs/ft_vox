#pragma once

#include <array>

#include <vulkan/vulkan.h>

#include "DeletionQueue.hpp"
#include "DescriptorAllocator.hpp"

class VulkanDevice;

class FrameManager {
  public:
    struct FrameData {
        VkCommandPool _commandPool{};
        VkCommandBuffer _mainCommandBuffer{};
        VkFence _renderFence{};
        DeletionQueue _deletionQueue;
        DescriptorAllocatorGrowable _frameDescriptors;
    };

    static constexpr unsigned int FRAME_OVERLAP = 2;

    explicit FrameManager(VulkanDevice& device);
    ~FrameManager();

    FrameManager(const FrameManager&) = delete;
    FrameManager& operator=(const FrameManager&) = delete;
    FrameManager(FrameManager&&) = delete;
    FrameManager& operator=(FrameManager&&) = delete;

    [[nodiscard]] FrameData& getCurrentFrame();
    [[nodiscard]] uint64_t getFrameNumber() const { return _frameNumber; }
    void incrementFrame() { _frameNumber++; }

  private:
    void createFrameCommandPools();
    void createFrameSyncStructures();
    void initFrameDescriptors();

    VulkanDevice& _device;
    uint64_t _frameNumber;
    std::array<FrameData, FRAME_OVERLAP> _frameData;
    DeletionQueue _frameDeletionQueue;
};

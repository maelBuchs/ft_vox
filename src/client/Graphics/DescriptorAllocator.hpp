#pragma once

#include <span>

#include <vulkan/vulkan.h>

class VulkanDevice;

class DescriptorAllocator {
  public:
    struct PoolSizeRatio {
        VkDescriptorType type;
        float ratio;
    };

    DescriptorAllocator(VulkanDevice& device, uint32_t maxSets,
                        std::span<PoolSizeRatio> poolRatios);
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;
    DescriptorAllocator(DescriptorAllocator&&) = delete;
    DescriptorAllocator& operator=(DescriptorAllocator&&) = delete;

    void clearDescriptors(VulkanDevice& device);
    VkDescriptorSet allocate(VulkanDevice& device, VkDescriptorSetLayout layout);

    [[nodiscard]] VkDescriptorPool getPool() const { return _pool; }

  private:
    VkDescriptorPool _pool = VK_NULL_HANDLE;
};

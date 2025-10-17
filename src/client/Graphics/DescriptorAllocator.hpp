#pragma once

#include <deque>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

// Growable descriptor allocator that creates new pools as needed
class DescriptorAllocatorGrowable {
  public:
    struct PoolSizeRatio {
        VkDescriptorType type;
        float ratio;
    };

    DescriptorAllocatorGrowable() = default;
    ~DescriptorAllocatorGrowable() = default;

    DescriptorAllocatorGrowable(const DescriptorAllocatorGrowable&) = delete;
    DescriptorAllocatorGrowable& operator=(const DescriptorAllocatorGrowable&) = delete;
    DescriptorAllocatorGrowable(DescriptorAllocatorGrowable&&) = delete;
    DescriptorAllocatorGrowable& operator=(DescriptorAllocatorGrowable&&) = delete;

    void init(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios);
    void clearPools(VkDevice device);
    void destroyPools(VkDevice device);

    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout, void* pNext = nullptr);

  private:
    VkDescriptorPool getPool(VkDevice device);
    VkDescriptorPool createPool(VkDevice device, uint32_t setCount,
                                std::span<PoolSizeRatio> poolRatios);

    std::vector<PoolSizeRatio> _ratios;
    std::vector<VkDescriptorPool> _fullPools;
    std::vector<VkDescriptorPool> _readyPools;
    uint32_t _setsPerPool = 0;
};

// Builder for creating descriptor set layouts
class DescriptorLayoutBuilder {
  public:
    DescriptorLayoutBuilder() = default;
    ~DescriptorLayoutBuilder() = default;

    DescriptorLayoutBuilder(const DescriptorLayoutBuilder&) = delete;
    DescriptorLayoutBuilder& operator=(const DescriptorLayoutBuilder&) = delete;
    DescriptorLayoutBuilder(DescriptorLayoutBuilder&&) = default;
    DescriptorLayoutBuilder& operator=(DescriptorLayoutBuilder&&) = default;

    void addBinding(uint32_t binding, VkDescriptorType type);
    void clear();
    VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shaderStages,
                                void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);

  private:
    std::vector<VkDescriptorSetLayoutBinding> _bindings;
};

// Writer for updating descriptor sets
class DescriptorWriter {
  public:
    DescriptorWriter() = default;
    ~DescriptorWriter() = default;

    DescriptorWriter(const DescriptorWriter&) = delete;
    DescriptorWriter& operator=(const DescriptorWriter&) = delete;
    DescriptorWriter(DescriptorWriter&&) = default;
    DescriptorWriter& operator=(DescriptorWriter&&) = default;

    void writeImage(int binding, VkImageView image, VkSampler sampler, VkImageLayout layout,
                    VkDescriptorType type);
    void writeBuffer(int binding, VkBuffer buffer, size_t size, size_t offset,
                     VkDescriptorType type);

    void clear();
    void updateSet(VkDevice device, VkDescriptorSet set);

  private:
    std::deque<VkDescriptorImageInfo> _imageInfos;
    std::deque<VkDescriptorBufferInfo> _bufferInfos;
    std::vector<VkWriteDescriptorSet> _writes;
};

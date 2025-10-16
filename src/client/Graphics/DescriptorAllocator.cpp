#include "DescriptorAllocator.hpp"

#include <stdexcept>
#include <vector>

#include "VulkanDevice.hpp"

DescriptorAllocator::DescriptorAllocator(VulkanDevice& device, uint32_t maxSets,
                                         std::span<PoolSizeRatio> poolRatios) {
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (const auto& ratio : poolRatios) {
        poolSizes.push_back(
            {.type = ratio.type,
             .descriptorCount = static_cast<uint32_t>(ratio.ratio * static_cast<float>(maxSets))});
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = 0;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device.getDevice(), &poolInfo, nullptr, &_pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
}

DescriptorAllocator::~DescriptorAllocator() {}

void DescriptorAllocator::clearDescriptors(VulkanDevice& device) {
    vkResetDescriptorPool(device.getDevice(), _pool, 0);
}

VkDescriptorSet DescriptorAllocator::allocate(VulkanDevice& device, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device.getDevice(), &allocInfo, &descriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor set");
    }
    return descriptorSet;
}

#include "DescriptorAllocator.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

void DescriptorAllocatorGrowable::init(VkDevice device, uint32_t initialSets,
                                       std::span<PoolSizeRatio> poolRatios) {
    _ratios.clear();

    for (auto r : poolRatios) {
        _ratios.push_back(r);
    }

    VkDescriptorPool newPool = createPool(device, initialSets, poolRatios);

    _setsPerPool = static_cast<uint32_t>(static_cast<float>(initialSets) * 1.5F);

    _readyPools.push_back(newPool);
}

void DescriptorAllocatorGrowable::clearPools(VkDevice device) {
    for (auto* p : _readyPools) {
        vkResetDescriptorPool(device, p, 0);
    }
    for (auto* p : _fullPools) {
        vkResetDescriptorPool(device, p, 0);
        _readyPools.push_back(p);
    }
    _fullPools.clear();
}

void DescriptorAllocatorGrowable::destroyPools(VkDevice device) {
    for (auto* p : _readyPools) {
        vkDestroyDescriptorPool(device, p, nullptr);
    }
    _readyPools.clear();
    for (auto* p : _fullPools) {
        vkDestroyDescriptorPool(device, p, nullptr);
    }
    _fullPools.clear();
}

VkDescriptorSet DescriptorAllocatorGrowable::allocate(VkDevice device, VkDescriptorSetLayout layout,
                                                      void* pNext) {
    // Get or create a pool to allocate from
    VkDescriptorPool poolToUse = getPool(device);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.pNext = pNext;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = poolToUse;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &ds);

    // Allocation failed. Try again
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
        _fullPools.push_back(poolToUse);

        poolToUse = getPool(device);
        allocInfo.descriptorPool = poolToUse;

        result = vkAllocateDescriptorSets(device, &allocInfo, &ds);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate descriptor set on second attempt");
        }
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor set");
    }

    _readyPools.push_back(poolToUse);
    return ds;
}

VkDescriptorPool DescriptorAllocatorGrowable::getPool(VkDevice device) {
    VkDescriptorPool newPool = VK_NULL_HANDLE;
    if (!_readyPools.empty()) {
        newPool = _readyPools.back();
        _readyPools.pop_back();
    } else {
        // Need to create a new pool
        newPool = createPool(device, _setsPerPool, _ratios);

        _setsPerPool = static_cast<uint32_t>(static_cast<float>(_setsPerPool) * 1.5F);
        _setsPerPool = std::min(_setsPerPool, 4092U);
    }

    return newPool;
}

VkDescriptorPool DescriptorAllocatorGrowable::createPool(VkDevice device, uint32_t setCount,
                                                         std::span<PoolSizeRatio> poolRatios) {
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (PoolSizeRatio ratio : poolRatios) {
        poolSizes.push_back(VkDescriptorPoolSize{
            .type = ratio.type,
            .descriptorCount = static_cast<uint32_t>(ratio.ratio * static_cast<float>(setCount))});
    }

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.maxSets = setCount;
    pool_info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool_info.pPoolSizes = poolSizes.data();

    VkDescriptorPool newPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &newPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
    return newPool;
}

void DescriptorLayoutBuilder::addBinding(uint32_t binding, VkDescriptorType type) {
    VkDescriptorSetLayoutBinding newBinding{};
    newBinding.binding = binding;
    newBinding.descriptorCount = 1;
    newBinding.descriptorType = type;

    _bindings.push_back(newBinding);
}

void DescriptorLayoutBuilder::clear() {
    _bindings.clear();
}

VkDescriptorSetLayout DescriptorLayoutBuilder::build(VkDevice device,
                                                     VkShaderStageFlags shaderStages, void* pNext,
                                                     VkDescriptorSetLayoutCreateFlags flags) {
    for (auto& b : _bindings) {
        b.stageFlags |= shaderStages;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = pNext;
    info.flags = flags;
    info.bindingCount = static_cast<uint32_t>(_bindings.size());
    info.pBindings = _bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }

    return layout;
}

void DescriptorWriter::writeImage(int binding, VkImageView image, VkSampler sampler,
                                  VkImageLayout layout, VkDescriptorType type) {
    VkDescriptorImageInfo& info = _imageInfos.emplace_back(
        VkDescriptorImageInfo{.sampler = sampler, .imageView = image, .imageLayout = layout});

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = static_cast<uint32_t>(binding);
    write.dstSet = VK_NULL_HANDLE; // Left empty for now until we need to write it
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = &info;

    _writes.push_back(write);
}

void DescriptorWriter::writeBuffer(int binding, VkBuffer buffer, size_t size, size_t offset,
                                   VkDescriptorType type) {
    VkDescriptorBufferInfo& info = _bufferInfos.emplace_back(
        VkDescriptorBufferInfo{.buffer = buffer, .offset = offset, .range = size});

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = static_cast<uint32_t>(binding);
    write.dstSet = VK_NULL_HANDLE; // Left empty for now until we need to write it
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &info;

    _writes.push_back(write);
}

void DescriptorWriter::clear() {
    _imageInfos.clear();
    _writes.clear();
    _bufferInfos.clear();
}

void DescriptorWriter::updateSet(VkDevice device, VkDescriptorSet set) {
    for (VkWriteDescriptorSet& write : _writes) {
        write.dstSet = set;
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(_writes.size()), _writes.data(), 0,
                           nullptr);
}

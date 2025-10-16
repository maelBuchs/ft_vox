#include "ComputePipelineBuilder.hpp"

#include <stdexcept>

#include "../Pipeline.hpp"
#include "../VulkanDevice.hpp"

ComputePipelineBuilder::ComputePipelineBuilder() {
    clear();
}

ComputePipelineBuilder::~ComputePipelineBuilder() {
    clear();
}

void ComputePipelineBuilder::clear() {
    _shaderPath.clear();
    _descriptorSetLayout = VK_NULL_HANDLE;
    _pushConstantRange = {};
    _hasPushConstants = false;
}

void ComputePipelineBuilder::setShader(std::string_view shaderPath) {
    _shaderPath = std::string(shaderPath);
}

void ComputePipelineBuilder::setDescriptorSetLayout(VkDescriptorSetLayout layout) {
    _descriptorSetLayout = layout;
}

void ComputePipelineBuilder::setPushConstantRange(VkPushConstantRange range) {
    _pushConstantRange = range;
    _hasPushConstants = true;
}

ComputePipelineBuilder::BuildResult ComputePipelineBuilder::build(VulkanDevice& device) {
    BuildResult result{};

    // Use provided descriptor set layout or VK_NULL_HANDLE
    result.descriptorSetLayout = _descriptorSetLayout;

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = (_descriptorSetLayout != VK_NULL_HANDLE) ? 1U : 0U,
        .pSetLayouts = (_descriptorSetLayout != VK_NULL_HANDLE) ? &_descriptorSetLayout : nullptr,
        .pushConstantRangeCount = _hasPushConstants ? 1U : 0U,
        .pPushConstantRanges = _hasPushConstants ? &_pushConstantRange : nullptr,
    };

    if (vkCreatePipelineLayout(device.getDevice(), &pipelineLayoutCreateInfo, nullptr,
                               &result.layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline layout");
    }

    // Load shader
    VkShaderModule computeShaderModule = Pipeline::loadShaderModule(device, _shaderPath);

    // Create shader stage
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = computeShaderModule,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };

    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = shaderStageCreateInfo,
        .layout = result.layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    if (vkCreateComputePipelines(device.getDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo,
                                 nullptr, &result.pipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(device.getDevice(), result.layout, nullptr);
        vkDestroyShaderModule(device.getDevice(), computeShaderModule, nullptr);
        throw std::runtime_error("Failed to create compute pipeline");
    }

    // Clean up shader module
    vkDestroyShaderModule(device.getDevice(), computeShaderModule, nullptr);

    return result;
}

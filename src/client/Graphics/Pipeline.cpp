#include "Pipeline.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "VulkanDevice.hpp"

Pipeline::Pipeline(VulkanDevice& device, std::string_view computeShaderPath) : _device(device) {

    VkDescriptorSetLayoutBinding storageImageBinding{.binding = 0,
                                                     .descriptorType =
                                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                     .descriptorCount = 1,
                                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                                     .pImmutableSamplers = nullptr};

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &storageImageBinding,
    };

    if (vkCreateDescriptorSetLayout(_device.getDevice(), &descriptorSetLayoutCreateInfo, nullptr,
                                    &_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }

    VkPushConstantRange pushConstant{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .offset = 0,
                                     .size = sizeof(ComputePushConstants)};

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &_descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstant,
    };

    if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutCreateInfo, nullptr,
                               &_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    VkShaderModule computeShaderModule = loadShaderModule(_device, computeShaderPath);

    VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = computeShaderModule,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };

    VkComputePipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = shaderStageCreateInfo,
        .layout = _pipelineLayout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    if (vkCreateComputePipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo,
                                 nullptr, &_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline");
    }

    vkDestroyShaderModule(_device.getDevice(), computeShaderModule, nullptr);
}

Pipeline::~Pipeline() {
    if (_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(_device.getDevice(), _pipeline, nullptr);
    }
    if (_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _pipelineLayout, nullptr);
    }
    if (_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _descriptorSetLayout, nullptr);
    }
}

VkShaderModule Pipeline::loadShaderModule(VulkanDevice& device, std::string_view shaderPath) {
    std::ifstream file(std::string(shaderPath), std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + std::string(shaderPath));
    }
    size_t fileSize = static_cast<size_t>(file.tellg());

    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);

    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));

    file.close();

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    createInfo.codeSize = buffer.size() * sizeof(uint32_t);
    createInfo.pCode = buffer.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device.getDevice(), &createInfo, nullptr, &shaderModule) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

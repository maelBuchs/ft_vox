#include "Pipeline.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "VulkanDevice.hpp"

void Pipeline::init(VkPipeline pipeline, VkPipelineLayout layout,
                    VkDescriptorSetLayout descriptorSetLayout) {
    _pipeline = pipeline;
    _pipelineLayout = layout;
    _descriptorSetLayout = descriptorSetLayout;
}

void Pipeline::cleanup(VulkanDevice& device) {
    if (_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device.getDevice(), _pipeline, nullptr);
        _pipeline = VK_NULL_HANDLE;
    }
    if (_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device.getDevice(), _pipelineLayout, nullptr);
        _pipelineLayout = VK_NULL_HANDLE;
    }
    if (_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device.getDevice(), _descriptorSetLayout, nullptr);
        _descriptorSetLayout = VK_NULL_HANDLE;
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

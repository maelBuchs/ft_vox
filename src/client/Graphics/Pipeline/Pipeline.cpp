#include "Pipeline.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "../Core/VulkanDevice.hpp"

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
}

VkShaderModule Pipeline::loadShaderModule(VulkanDevice& device, std::string_view shaderPath) {
    std::cout << "[SHADER] Loading shader module from: " << shaderPath << "\n";
    std::flush(std::cout);

    std::ifstream file(std::string(shaderPath), std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        std::cerr << "[SHADER ERROR] Failed to open shader file: " << shaderPath << "\n";
        throw std::runtime_error("Failed to open shader file: " + std::string(shaderPath));
    }
    size_t fileSize = static_cast<size_t>(file.tellg());

    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);

    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));

    file.close();

    std::cout << "[SHADER] Shader file loaded\n";
    std::cout << "  - Code size: " << (buffer.size() * sizeof(uint32_t)) << " bytes\n";
    std::cout << "  - SPIR-V words: " << buffer.size() << "\n";

    // Validate SPIR-V format
    if (buffer.size() < 5) {
        std::cerr << "[SHADER ERROR] SPIR-V file too small - may be corrupted!\n";
        std::cerr << "  - File: " << shaderPath << "\n";
        std::cerr << "  - Size: " << buffer.size() << " words (minimum 5 required)\n";
        throw std::runtime_error("Invalid SPIR-V file (too small): " + std::string(shaderPath));
    }

    // Check SPIR-V magic number (0x07230203)
    if (buffer[0] != 0x07230203) {
        std::cerr << "[SHADER ERROR] Invalid SPIR-V magic number!\n";
        std::cerr << "  - File: " << shaderPath << "\n";
        std::cerr << "  - Expected: 0x07230203\n";
        std::cerr << "  - Found: 0x" << std::hex << buffer[0] << std::dec << "\n";
        throw std::runtime_error("Corrupted SPIR-V file (invalid magic): " + std::string(shaderPath));
    }

    std::cout << "[SHADER] SPIR-V validation passed (magic: 0x" << std::hex << buffer[0] << std::dec << ")\n";
    std::flush(std::cout);

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    createInfo.codeSize = buffer.size() * sizeof(uint32_t);
    createInfo.pCode = buffer.data();

    std::cout << "[SHADER] Creating shader module...\n";
    std::flush(std::cout);

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device.getDevice(), &createInfo, nullptr, &shaderModule) !=
        VK_SUCCESS) {
        std::cerr << "[SHADER ERROR] vkCreateShaderModule failed for: " << shaderPath << "\n";
        throw std::runtime_error("Failed to create shader module: " + std::string(shaderPath));
    }

    std::cout << "[SHADER] Shader module created successfully: " << shaderModule << "\n";
    std::flush(std::cout);

    return shaderModule;
}

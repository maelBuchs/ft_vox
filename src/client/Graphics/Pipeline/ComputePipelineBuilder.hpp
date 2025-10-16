#pragma once

#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

class VulkanDevice;

class ComputePipelineBuilder {
  public:
    ComputePipelineBuilder();
    ~ComputePipelineBuilder();

    ComputePipelineBuilder(const ComputePipelineBuilder&) = delete;
    ComputePipelineBuilder& operator=(const ComputePipelineBuilder&) = delete;
    ComputePipelineBuilder(ComputePipelineBuilder&&) = delete;
    ComputePipelineBuilder& operator=(ComputePipelineBuilder&&) = delete;

    void clear();
    void setShader(std::string_view shaderPath);
    void setDescriptorSetLayout(VkDescriptorSetLayout layout);
    void setPushConstantRange(VkPushConstantRange range);

    struct BuildResult {
        VkPipeline pipeline;
        VkPipelineLayout layout;
        VkDescriptorSetLayout descriptorSetLayout;
    };
    BuildResult build(VulkanDevice& device);

  private:
    std::string _shaderPath;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
    VkPushConstantRange _pushConstantRange = {};
    bool _hasPushConstants = false;
};

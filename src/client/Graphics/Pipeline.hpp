#pragma once

#include <string_view>

#include <vulkan/vulkan.h>

class VulkanDevice;

class Pipeline {
  public:
    Pipeline(VulkanDevice& device, std::string_view computeShaderPath);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    [[nodiscard]] VkPipeline getPipeline() const { return _pipeline; }
    [[nodiscard]] VkPipelineLayout getLayout() const { return _pipelineLayout; }
    [[nodiscard]] VkDescriptorSetLayout getDescriptorSetLayout() const {
        return _descriptorSetLayout;
    }

  private:
    VkShaderModule loadShaderModule(std::string_view computeShaderPath);

    VulkanDevice& _device;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
};

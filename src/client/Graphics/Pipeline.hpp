#pragma once

#include <string_view>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

class VulkanDevice;

struct ComputePushConstants {
    glm::vec4 data1;
    glm::vec4 data2;
    glm::vec4 data3;
    glm::vec4 data4;
};

class Pipeline {
  public:
    Pipeline() = default;
    ~Pipeline() = default;

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = default;
    Pipeline& operator=(Pipeline&&) = default;

    void init(VkPipeline pipeline, VkPipelineLayout layout,
              VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE);
    void cleanup(VulkanDevice& device);

    [[nodiscard]] VkPipeline getPipeline() const { return _pipeline; }
    [[nodiscard]] VkPipelineLayout getLayout() const { return _pipelineLayout; }
    [[nodiscard]] VkDescriptorSetLayout getDescriptorSetLayout() const {
        return _descriptorSetLayout;
    }

    static VkShaderModule loadShaderModule(VulkanDevice& device, std::string_view shaderPath);

  private:
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
};

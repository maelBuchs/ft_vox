#pragma once

#include <functional>

#include <vulkan/vulkan.h>

class VulkanDevice;
class RenderContext;

class CommandExecutor {
  public:
    static constexpr uint64_t VULKAN_TIMEOUT_NS = 1000000000; // 1 second

    CommandExecutor(VulkanDevice& device, RenderContext& context);
    ~CommandExecutor() = default;

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;
    CommandExecutor(CommandExecutor&&) = delete;
    CommandExecutor& operator=(CommandExecutor&&) = delete;

    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout) const;
    void copyImageToImage(VkCommandBuffer cmd, VkImage source, VkImage destination,
                          VkExtent2D srcSize, VkExtent2D dstSize);
    void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);

  private:
    [[nodiscard]] static VkImageMemoryBarrier2
    createImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    [[nodiscard]] static VkImageAspectFlags getImageAspectMask(VkImageLayout layout);

    VulkanDevice& _device;
    RenderContext& _context;
};

#pragma once

#include <vector>

#include "VulkanDevice.hpp"

class Window;

class VulkanSwapchain {
  public:
    VulkanSwapchain(Window& window, VulkanDevice& device);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
    VulkanSwapchain(VulkanSwapchain&&) = delete;
    VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;

    [[nodiscard]] VkSwapchainKHR getSwapchain() const { return _swapchain; }
    [[nodiscard]] VkFormat getSwapchainImageFormat() const { return _swapchainImageFormat; }
    [[nodiscard]] const std::vector<VkImage>& getSwapchainImages() const {
        return _swapchainImages;
    }
    [[nodiscard]] const std::vector<VkImageView>& getSwapchainImageViews() const {
        return _swapchainImageViews;
    }
    [[nodiscard]] VkExtent2D getSwapchainExtent() const { return _swapchainExtent; }

  private:
    VulkanDevice& _device;
    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;
    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;
};

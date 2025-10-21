#include "VulkanSwapchain.hpp"

#include <vulkan/vulkan.h>

#include "client/Core/Window.hpp"
#include "VkBootstrap.h"
#include "VulkanDevice.hpp"

VulkanSwapchain::VulkanSwapchain(Window& window, VulkanDevice& device)
    : _device(device), _swapchainImageFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32) {

    vkb::SwapchainBuilder swapchainBuilder{_device.getPhysicalDevice(), _device.getDevice(),
                                           _device.getSurface()};

    VkSurfaceFormatKHR desiredFormat10bit{
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    };

    VkSurfaceFormatKHR desiredFormat8bit{
        .format = VK_FORMAT_B8G8R8A8_SRGB,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    };

    auto swapchainResult = swapchainBuilder.set_desired_format(desiredFormat10bit)
                               .add_fallback_format(desiredFormat8bit)
                               .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                               .set_desired_extent(static_cast<uint32_t>(window.getWidth()),
                                                   static_cast<uint32_t>(window.getHeight()))
                               .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                               .build();

    if (!swapchainResult) {
        throw std::runtime_error("Failed to create swapchain: " +
                                 swapchainResult.error().message());
    }
    vkb::Swapchain vkbSwapchain = swapchainResult.value();

    _swapchainExtent = vkbSwapchain.extent;
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

VulkanSwapchain::~VulkanSwapchain() {
    vkDestroySwapchainKHR(_device.getDevice(), _swapchain, nullptr);
    for (size_t i = 0; i < _swapchainImageViews.size(); i++) {
        vkDestroyImageView(_device.getDevice(), _swapchainImageViews[i], nullptr);
    }
}

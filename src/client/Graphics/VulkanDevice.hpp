#pragma once

#include <SDL3/SDL_video.h>
#include <vulkan/vulkan.h>

class VulkanDevice {
  public:
    VulkanDevice(SDL_Window* window);
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    VulkanDevice(VulkanDevice&&) = delete;
    VulkanDevice& operator=(VulkanDevice&&) = delete;

    [[nodiscard]] VkInstance getInstance() const { return _instance; }
    [[nodiscard]] VkDebugUtilsMessengerEXT getDebugMessenger() const { return _debugMessenger; }
    [[nodiscard]] VkSurfaceKHR getSurface() const { return _surface; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return _physicalDevice; }
    [[nodiscard]] VkDevice getDevice() const { return _device; }
    [[nodiscard]] VkQueue getQueue() const { return _graphicsQueue; }
    [[nodiscard]] uint32_t getGraphicsQueueFamily() const { return _graphicsQueueFamily; }

  private:
    VkInstance _instance;
    VkDebugUtilsMessengerEXT _debugMessenger;
    VkSurfaceKHR _surface;
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;
    VkQueue _graphicsQueue;
    uint32_t _graphicsQueueFamily;
};

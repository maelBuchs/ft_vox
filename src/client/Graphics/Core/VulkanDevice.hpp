#pragma once

#include <vk_mem_alloc.h>

#include <SDL3/SDL_video.h>
#include <tracy/TracyVulkan.hpp>
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
    [[nodiscard]] VmaAllocator getAllocator() const { return _allocator; }
    [[nodiscard]] TracyVkCtx getTracyCtx() const { return _tracyCtx; }

    [[nodiscard]] bool hasAsyncTransferQueue() const { return _hasAsyncTransfer; }
    [[nodiscard]] VkQueue getTransferQueue() const { return _transferQueue; }
    [[nodiscard]] uint32_t getTransferQueueFamily() const { return _transferQueueFamily; }

    // Mesh shader support query
    [[nodiscard]] bool supportsMeshShaders() const { return _meshShaderSupported; }

    // Get current VRAM usage statistics
    struct VRAMStats {
        uint64_t usedBytes;
        uint64_t budgetBytes;
        uint32_t allocationCount;
    };
    [[nodiscard]] VRAMStats getVRAMStats() const;

    void logDeviceFaultInfo(const char* context) const;

  private:
    VkInstance _instance;
    VkDebugUtilsMessengerEXT _debugMessenger;
    VkSurfaceKHR _surface;
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;
    VkQueue _graphicsQueue;
    uint32_t _graphicsQueueFamily;
    VmaAllocator _allocator;
    VkCommandPool _tracyCommandPool;
    VkCommandBuffer _tracyCommandBuffer;
    TracyVkCtx _tracyCtx;
    bool _meshShaderSupported;
    bool _supportsDeviceFault;
    PFN_vkGetDeviceFaultInfoEXT _getDeviceFaultInfo;

    bool _hasAsyncTransfer;
    VkQueue _transferQueue;
    uint32_t _transferQueueFamily;
};

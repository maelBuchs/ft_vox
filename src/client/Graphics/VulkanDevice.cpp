#include "VulkanDevice.hpp"

#include <VkBootstrap.h>

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

VulkanDevice::VulkanDevice(SDL_Window* window)
    : _instance(nullptr), _debugMessenger(nullptr), _surface(nullptr), _physicalDevice(nullptr),
      _device(nullptr), _graphicsQueue(nullptr) {

    vkb::InstanceBuilder instanceBuilder;
    auto instRet = instanceBuilder.set_app_name("ft_vox")
                       .request_validation_layers(true)
                       .use_default_debug_messenger()
                       .require_api_version(1, 3)
                       .build();

    if (!instRet) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    vkb::Instance vkbInstance = instRet.value();
    _instance = vkbInstance.instance;
    _debugMessenger = vkbInstance.debug_messenger;

    if (!SDL_Vulkan_CreateSurface(window, _instance, nullptr, &_surface)) {
        throw std::runtime_error("Failed to create Vulkan surface");
    }

    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE};

    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .descriptorIndexing = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE};

    vkb::PhysicalDeviceSelector selector{vkbInstance, _surface};

    auto physicalDeviceRet = selector.set_minimum_version(1, 3)
                                 .set_required_features_12(features12)
                                 .set_required_features_13(features13)
                                 .select();

    if (!physicalDeviceRet) {
        throw std::runtime_error("Failed to select physical device: " +
                                 physicalDeviceRet.error().message());
    }

    const vkb::PhysicalDevice& vkbPhysicalDevice = physicalDeviceRet.value();
    _physicalDevice = vkbPhysicalDevice.physical_device;

    vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
    auto deviceRet = deviceBuilder.build();

    if (!deviceRet) {
        throw std::runtime_error("Failed to create logical device: " + deviceRet.error().message());
    }

    const vkb::Device& vkbDevice = deviceRet.value();
    _device = vkbDevice.device;

    auto queueRet = vkbDevice.get_queue(vkb::QueueType::graphics);
    if (!queueRet) {
        throw std::runtime_error("Failed to get graphics queue: " + queueRet.error().message());
    }

    _graphicsQueue = queueRet.value();
}

VulkanDevice::~VulkanDevice() {

    if (_surface != nullptr) {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
    }

    if (_device != nullptr) {
        vkDestroyDevice(_device, nullptr);
    }

    if (_debugMessenger != nullptr) {
        vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
    }

    if (_instance != nullptr) {
        vkDestroyInstance(_instance, nullptr);
    }
}

#define VMA_IMPLEMENTATION

// Enable Tracy memory tracking for VMA
#define VMA_RECORDING_ENABLED 1
#define VMA_DEDICATED_ALLOCATION 1
#define VMA_DEBUG_MARGIN 0

#include "VulkanDevice.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <VkBootstrap.h>

#include <SDL3/SDL_vulkan.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#include <vulkan/vulkan.h>

// VMA callbacks for Tracy memory tracking
namespace {
void vmaAllocationCallback(VmaAllocator allocator, uint32_t memoryType, VkDeviceMemory memory,
                           VkDeviceSize size, void* pUserData) {
    // Track VRAM allocation in Tracy
    TracyAllocN(reinterpret_cast<void*>(static_cast<uintptr_t>(reinterpret_cast<uint64_t>(memory))),
                size, "VRAM");
}

void vmaDeallocationCallback(VmaAllocator allocator, uint32_t memoryType, VkDeviceMemory memory,
                             VkDeviceSize size, void* pUserData) {
    // Track VRAM deallocation in Tracy
    TracyFreeN(reinterpret_cast<void*>(static_cast<uintptr_t>(reinterpret_cast<uint64_t>(memory))),
               "VRAM");
}
} // namespace

VulkanDevice::VulkanDevice(SDL_Window* window)
    : _instance(nullptr), _debugMessenger(nullptr), _surface(nullptr), _physicalDevice(nullptr),
      _device(nullptr), _graphicsQueue(nullptr), _allocator(nullptr), _tracyCommandPool(nullptr),
      _tracyCommandBuffer(nullptr), _tracyCtx(nullptr), _meshShaderSupported(false),
      _hasAsyncTransfer(false), _transferQueue(nullptr), _transferQueueFamily(UINT32_MAX) {

    vkb::InstanceBuilder instanceBuilder;
    auto instRet = instanceBuilder.set_app_name("ft_vox")
                       .request_validation_layers(true)
                       .use_default_debug_messenger()
                       .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT)
                       .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT)
                       .add_validation_feature_enable(
                           VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
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
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE}; // Required for LocalSizeId in mesh shaders

    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .drawIndirectCount = VK_TRUE,
        .descriptorIndexing = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE};

    VkPhysicalDeviceVulkan11Features features11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = VK_TRUE};

    // Enable wireframe, multiDrawIndirect, and shaderInt64 support
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.fillModeNonSolid = VK_TRUE;
    deviceFeatures.multiDrawIndirect = VK_TRUE;
    deviceFeatures.shaderInt64 =
        VK_TRUE; // REQUIRED for uint64_t in shaders (buffer device address)

    vkb::PhysicalDeviceSelector selector{vkbInstance, _surface};

    auto physicalDeviceRet = selector.set_minimum_version(1, 3)
                                 .set_required_features(deviceFeatures)
                                 .set_required_features_11(features11)
                                 .set_required_features_12(features12)
                                 .set_required_features_13(features13)
                                 .select();

    if (!physicalDeviceRet) {
        throw std::runtime_error("Failed to select physical device: " +
                                 physicalDeviceRet.error().message());
    }

    const vkb::PhysicalDevice& vkbPhysicalDevice = physicalDeviceRet.value();
    _physicalDevice = vkbPhysicalDevice.physical_device;

    // Check Vulkan version support (mesh shaders require maintenance4 which is in Vulkan 1.3)
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(_physicalDevice, &deviceProps);

    bool vulkan13Supported = (deviceProps.apiVersion >= VK_API_VERSION_1_3);
    if (!vulkan13Supported) {
        std::cout << "[VulkanDevice] Vulkan 1.3 not supported (found version "
                  << VK_API_VERSION_MAJOR(deviceProps.apiVersion) << "."
                  << VK_API_VERSION_MINOR(deviceProps.apiVersion) << "."
                  << VK_API_VERSION_PATCH(deviceProps.apiVersion)
                  << "), disabling mesh shaders\n";
        _meshShaderSupported = false;
    }

    // Declare structures that will be used later if mesh shaders are supported
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
    meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    meshShaderFeatures.pNext = nullptr;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    // Only check for mesh shader extension and features if Vulkan 1.3 is supported
    if (vulkan13Supported) {
        // Check if VK_EXT_mesh_shader extension is available
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &extensionCount,
                                             availableExtensions.data());

        bool meshShaderExtAvailable = false;
        for (const auto& ext : availableExtensions) {
            if (strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
                meshShaderExtAvailable = true;
                break;
            }
        }

        if (!meshShaderExtAvailable) {
            std::cout << "[VulkanDevice] VK_EXT_mesh_shader extension not available, disabling mesh "
                         "shader support\n";
            _meshShaderSupported = false;
        } else {
            // Check for mesh shader support
            deviceFeatures2.pNext = &meshShaderFeatures;
            vkGetPhysicalDeviceFeatures2(_physicalDevice, &deviceFeatures2);

            // Enable mesh shaders if supported
            _meshShaderSupported =
                (meshShaderFeatures.taskShader == VK_TRUE && meshShaderFeatures.meshShader == VK_TRUE);

            if (!_meshShaderSupported) {
                std::cout << "[VulkanDevice] Mesh shader features not supported by device\n";
            }
        }
    }

    if (_meshShaderSupported) {
        // Reset mesh shader features for device creation
        meshShaderFeatures.taskShader = VK_TRUE;
        meshShaderFeatures.meshShader = VK_TRUE;
        meshShaderFeatures.multiviewMeshShader = VK_FALSE;
        meshShaderFeatures.primitiveFragmentShadingRateMeshShader = VK_FALSE;
        meshShaderFeatures.meshShaderQueries = VK_FALSE;

        // Get queue family indices
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount,
                                                 queueFamilies.data());

        uint32_t graphicsFamily = UINT32_MAX;
        uint32_t transferFamily = UINT32_MAX;

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsFamily = i;
            }
            // Look for dedicated transfer queue (transfer but NOT graphics)
            if ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                transferFamily = i;
            }
        }

        if (graphicsFamily == UINT32_MAX) {
            throw std::runtime_error("Failed to find graphics queue family");
        }

        if (transferFamily == UINT32_MAX) {
            transferFamily = graphicsFamily;
        } else {
            _hasAsyncTransfer = true;
        }

        float queuePriorities[] = {1.0f, 1.0f};
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

        // Graphics queue
        VkDeviceQueueCreateInfo graphicsQueueInfo{};
        graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        graphicsQueueInfo.queueFamilyIndex = graphicsFamily;
        graphicsQueueInfo.queueCount = 1;
        graphicsQueueInfo.pQueuePriorities = &queuePriorities[0];
        queueCreateInfos.push_back(graphicsQueueInfo);

        // Transfer queue (if different from graphics)
        if (transferFamily != graphicsFamily) {
            VkDeviceQueueCreateInfo transferQueueInfo{};
            transferQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            transferQueueInfo.queueFamilyIndex = transferFamily;
            transferQueueInfo.queueCount = 1;
            transferQueueInfo.pQueuePriorities = &queuePriorities[1];
            queueCreateInfos.push_back(transferQueueInfo);
        }

        // Build extension list
        std::vector<const char*> deviceExts = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_MESH_SHADER_EXTENSION_NAME // ADD THIS!
        };

        // Build pNext chain
        features13.pNext = &meshShaderFeatures;
        features12.pNext = &features13;
        features11.pNext = &features12;
        deviceFeatures2.pNext = &features11;
        deviceFeatures2.features = deviceFeatures;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &deviceFeatures2;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        createInfo.ppEnabledExtensionNames = deviceExts.data();

        VkResult result = vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device);
        if (result != VK_SUCCESS) {
            std::cerr << "[VulkanDevice] Failed to create device with mesh shader support (VkResult: "
                      << result << "). Falling back to non-mesh-shader path.\n";
            _meshShaderSupported = false;
        } else {
            std::cout << "[VulkanDevice] Successfully created device with mesh shader support\n";
            _graphicsQueueFamily = graphicsFamily;
            _transferQueueFamily = transferFamily;
            vkGetDeviceQueue(_device, graphicsFamily, 0, &_graphicsQueue);
            vkGetDeviceQueue(_device, transferFamily, 0, &_transferQueue);
        }
    }

    if (!_meshShaderSupported) {
        vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
        auto deviceRet = deviceBuilder.build();

        if (!deviceRet) {
            throw std::runtime_error("Failed to create logical device: " +
                                     deviceRet.error().message());
        }

        const vkb::Device& vkbDevice = deviceRet.value();
        _device = vkbDevice.device;

        auto queueRet = vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueRet) {
            throw std::runtime_error("Failed to get graphics queue: " + queueRet.error().message());
        }
        _graphicsQueue = queueRet.value();

        auto queueFamilyRet = vkbDevice.get_queue_index(vkb::QueueType::graphics);
        if (!queueFamilyRet) {
            throw std::runtime_error("Failed to get graphics queue family index: " +
                                     queueFamilyRet.error().message());
        }
        _graphicsQueueFamily = queueFamilyRet.value();

        auto transferQueueRet = vkbDevice.get_queue(vkb::QueueType::transfer);
        auto transferFamilyRet = vkbDevice.get_queue_index(vkb::QueueType::transfer);

        if (transferQueueRet && transferFamilyRet) {
            _transferQueue = transferQueueRet.value();
            _transferQueueFamily = transferFamilyRet.value();
            _hasAsyncTransfer = (_transferQueueFamily != _graphicsQueueFamily);
        } else {
            _transferQueue = _graphicsQueue;
            _transferQueueFamily = _graphicsQueueFamily;
            _hasAsyncTransfer = false;
        }
    }

    if (_meshShaderSupported) {
        auto testFnPtr = vkGetDeviceProcAddr(_device, "vkCmdDrawMeshTasksEXT");
        if (testFnPtr == nullptr) {
            _meshShaderSupported = false;
        }
    }

    // Setup VMA device memory callbacks for Tracy profiling
    VmaDeviceMemoryCallbacks vmaCallbacks{};
    vmaCallbacks.pfnAllocate = vmaAllocationCallback;
    vmaCallbacks.pfnFree = vmaDeallocationCallback;
    vmaCallbacks.pUserData = nullptr;

    VmaAllocatorCreateInfo allocatorInfo = {.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
                                            .physicalDevice = _physicalDevice,
                                            .device = _device,
                                            .pDeviceMemoryCallbacks = &vmaCallbacks,
                                            .instance = _instance};
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    // Create Tracy command pool and buffer
    VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = _graphicsQueueFamily};
    vkCreateCommandPool(_device, &poolInfo, nullptr, &_tracyCommandPool);

    VkCommandBufferAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = _tracyCommandPool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
    vkAllocateCommandBuffers(_device, &allocInfo, &_tracyCommandBuffer);

    _tracyCtx = TracyVkContext(_physicalDevice, _device, _graphicsQueue, _tracyCommandBuffer);
}

VulkanDevice::~VulkanDevice() {
    if (_device != nullptr) {
        vkDeviceWaitIdle(_device);
    }
    if (_tracyCtx != nullptr) {
        TracyVkDestroy(_tracyCtx);
    }

    if (_tracyCommandPool != nullptr) {
        vkDestroyCommandPool(_device, _tracyCommandPool, nullptr);
    }

    if (_allocator != nullptr) {
        VmaTotalStatistics stats;
        vmaCalculateStatistics(_allocator, &stats);

        if (stats.total.statistics.allocationCount > 0) {
            char* statsString = nullptr;
            vmaBuildStatsString(_allocator, &statsString, VK_TRUE);
            if (statsString != nullptr) {
                std::ofstream outFile("vma_leak_stats.json");
                if (outFile.is_open()) {
                    outFile << statsString;
                    outFile.close();
                }
                vmaFreeStatsString(_allocator, statsString);
            }
        }

        vmaDestroyAllocator(_allocator);
    }
    if (_device != nullptr) {
        vkDestroyDevice(_device, nullptr);
    }

    if (_surface != nullptr) {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
    }

    if (_debugMessenger != nullptr) {
        vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
    }

    if (_instance != nullptr) {
        vkDestroyInstance(_instance, nullptr);
    }
}

VulkanDevice::VRAMStats VulkanDevice::getVRAMStats() const {
    VmaTotalStatistics stats;
    vmaCalculateStatistics(_allocator, &stats);

    VRAMStats result{};
    result.usedBytes = stats.total.statistics.allocationBytes;
    result.budgetBytes = stats.total.statistics.blockBytes;
    result.allocationCount = stats.total.statistics.allocationCount;

    return result;
}

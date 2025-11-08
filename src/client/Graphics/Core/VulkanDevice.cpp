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

    // Check for mesh shader support
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
    meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    meshShaderFeatures.pNext = nullptr;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = &meshShaderFeatures;

    vkGetPhysicalDeviceFeatures2(_physicalDevice, &deviceFeatures2);

    // Enable mesh shaders if supported
    _meshShaderSupported =
        (meshShaderFeatures.taskShader == VK_TRUE && meshShaderFeatures.meshShader == VK_TRUE);

    if (_meshShaderSupported) {
        std::cout << "[VulkanDevice] Mesh shader support detected - creating device with mesh "
                     "shader extension\\n";

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

        // PHASE 2: Find graphics and dedicated transfer queue families
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

        // If no dedicated transfer queue, use graphics queue for transfers
        if (transferFamily == UINT32_MAX) {
            std::cout << "[VulkanDevice] No dedicated transfer queue found, using graphics queue\n";
            transferFamily = graphicsFamily;
        } else {
            std::cout << "[VulkanDevice] Found dedicated transfer queue family: " << transferFamily << "\n";
            _hasAsyncTransfer = true;
        }

        // PHASE 2: Create queue create infos for both queues
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
            std::cout << "[VulkanDevice] WARN: Failed to create device with mesh shaders (error "
                      << result << "), falling back\n";
            _meshShaderSupported = false;
        } else {
            std::cout << "[VulkanDevice] Device created with VK_EXT_mesh_shader extension\n";
            _graphicsQueueFamily = graphicsFamily;
            _transferQueueFamily = transferFamily;
            vkGetDeviceQueue(_device, graphicsFamily, 0, &_graphicsQueue);
            vkGetDeviceQueue(_device, transferFamily, 0, &_transferQueue);
            std::cout << "[VulkanDevice] Graphics queue family: " << graphicsFamily << "\n";
            std::cout << "[VulkanDevice] Transfer queue family: " << transferFamily << "\n";
        }
    }

    // If mesh shaders failed or not supported, use vk-bootstrap
    if (!_meshShaderSupported) {
        std::cout << "[VulkanDevice] Creating device without mesh shaders\\n";
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

        // PHASE 2: Try to get transfer queue (fallback path)
        auto transferQueueRet = vkbDevice.get_queue(vkb::QueueType::transfer);
        auto transferFamilyRet = vkbDevice.get_queue_index(vkb::QueueType::transfer);

        if (transferQueueRet && transferFamilyRet) {
            _transferQueue = transferQueueRet.value();
            _transferQueueFamily = transferFamilyRet.value();
            _hasAsyncTransfer = (_transferQueueFamily != _graphicsQueueFamily);
            std::cout << "[VulkanDevice] Transfer queue family: " << _transferQueueFamily
                      << ((_hasAsyncTransfer) ? " (dedicated)" : " (shared with graphics)") << "\n";
        } else {
            // Fallback: use graphics queue for transfers
            _transferQueue = _graphicsQueue;
            _transferQueueFamily = _graphicsQueueFamily;
            _hasAsyncTransfer = false;
            std::cout << "[VulkanDevice] Using graphics queue for transfers\n";
        }
    }

    // Debug: Try to load the mesh shader function pointer to verify extension is enabled
    if (_meshShaderSupported) {
        auto testFnPtr = vkGetDeviceProcAddr(_device, "vkCmdDrawMeshTasksEXT");
        if (testFnPtr == nullptr) {
            std::cout
                << "[VulkanDevice] WARNING: vkCmdDrawMeshTasksEXT function pointer is NULL\\n";
            std::cout << "[VulkanDevice] This means VK_EXT_mesh_shader extension is NOT enabled\\n";
            std::cout << "[VulkanDevice] Disabling mesh shader support\\n";
            _meshShaderSupported = false;
        } else {
            std::cout << "[VulkanDevice] SUCCESS: vkCmdDrawMeshTasksEXT function pointer loaded\\n";
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
    auto startTime = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] Destructor start\n";

    // CRITICAL: Wait for device idle BEFORE destroying Tracy or VMA
    // This ensures all GPU operations complete before we free resources
    auto t1 = std::chrono::high_resolution_clock::now();
    if (_device != nullptr) {
        vkDeviceWaitIdle(_device);
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] vkDeviceWaitIdle took: "
              << std::chrono::duration<double, std::milli>(t2 - t1).count() << "ms\n";

    auto t3 = std::chrono::high_resolution_clock::now();
    if (_tracyCtx != nullptr) {
        TracyVkDestroy(_tracyCtx);
    }

    if (_tracyCommandPool != nullptr) {
        vkDestroyCommandPool(_device, _tracyCommandPool, nullptr);
    }
    auto t4 = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] Tracy cleanup took: "
              << std::chrono::duration<double, std::milli>(t4 - t3).count() << "ms\n";

    // No need to wait again - TracyVkDestroy doesn't queue GPU work

    auto t5 = std::chrono::high_resolution_clock::now();
    if (_allocator != nullptr) {
        // Dump VMA statistics before destroying to see what's still allocated
        VmaTotalStatistics stats;
        vmaCalculateStatistics(_allocator, &stats);

        if (stats.total.statistics.allocationCount > 0) {
            std::cout << "[VulkanDevice] WARNING: " << stats.total.statistics.allocationCount
                      << " VMA allocations still remain! These are leaked! ("
                      << (stats.total.statistics.allocationBytes / 1024 / 1024) << " MB)\n";

            // Dump JSON statistics to file for detailed analysis
            char* statsString = nullptr;
            vmaBuildStatsString(_allocator, &statsString, VK_TRUE);
            if (statsString != nullptr) {
                std::ofstream outFile("vma_leak_stats.json");
                if (outFile.is_open()) {
                    outFile << statsString;
                    outFile.close();
                    std::cout << "[VulkanDevice] Wrote detailed VMA stats to vma_leak_stats.json\n";
                }
                vmaFreeStatsString(_allocator, statsString);
            }
        }

        vmaDestroyAllocator(_allocator);
    }
    auto t6 = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] VMA allocator destruction took: "
              << std::chrono::duration<double, std::milli>(t6 - t5).count() << "ms\n";

    auto t7 = std::chrono::high_resolution_clock::now();
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
    auto t8 = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] Vulkan cleanup took: "
              << std::chrono::duration<double, std::milli>(t8 - t7).count() << "ms\n";

    auto endTime = std::chrono::high_resolution_clock::now();
    std::cout << "[VulkanDevice] Total destructor time: "
              << std::chrono::duration<double, std::milli>(endTime - startTime).count() << "ms\n";
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

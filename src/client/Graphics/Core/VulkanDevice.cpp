#define VMA_IMPLEMENTATION

// Enable Tracy memory tracking for VMA
#define VMA_RECORDING_ENABLED 1
#define VMA_DEDICATED_ALLOCATION 1
#define VMA_DEBUG_MARGIN 0

#include "VulkanDevice.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <VkBootstrap.h>

#include <SDL3/SDL_vulkan.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#include <vulkan/vulkan.h>

// VMA callbacks for Tracy memory tracking
namespace {
bool hasDeviceExtension(VkPhysicalDevice device, const char* extensionName) {
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                                  extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, extensionName) == 0) {
            return true;
        }
    }

    return false;
}

const char* deviceFaultAddressTypeToString(VkDeviceFaultAddressTypeEXT addressType) {
    switch (addressType) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        return "NONE";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        return "READ_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        return "WRITE_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        return "EXECUTE_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "INSTRUCTION_POINTER_UNKNOWN";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "INSTRUCTION_POINTER_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "INSTRUCTION_POINTER_FAULT";
    default:
        return "UNKNOWN";
    }
}

void vmaAllocationCallback(VmaAllocator allocator, uint32_t memoryType, VkDeviceMemory memory,
                           VkDeviceSize size, void* pUserData) {
    // Track VRAM allocation in Tracy
    TracyAllocN(reinterpret_cast<void*>(static_cast<uintptr_t>(reinterpret_cast<uint64_t>(memory))),
                size, "VRAM");
}

const char* debugSeverityToString(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        return "VERBOSE";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        return "INFO";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        return "WARNING";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* /*userData*/) {
    // Filter out BestPractices warnings for small dedicated allocations.
    // VMA correctly falls back to dedicated allocations when the driver requests it
    // (e.g. NVIDIA drivers for VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT), making
    // these Validation Layer warnings a false positive for our architecture.
    if (callbackData->messageIdNumber == 280337739 || callbackData->messageIdNumber == 1147161417) {
        return VK_FALSE;
    }

    std::ostream& output = (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                               ? std::cerr
                               : std::cout;

    output << "[Vulkan][" << debugSeverityToString(messageSeverity) << "][";

    bool firstType = true;
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0U) {
        output << "GENERAL";
        firstType = false;
    }
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0U) {
        if (!firstType) {
            output << '|';
        }
        output << "VALIDATION";
        firstType = false;
    }
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0U) {
        if (!firstType) {
            output << '|';
        }
        output << "PERFORMANCE";
        firstType = false;
    }
    if (firstType) {
        output << "UNKNOWN";
    }

    output << "] ";

    if (callbackData->pMessageIdName != nullptr) {
        output << callbackData->pMessageIdName << " (" << callbackData->messageIdNumber << ")";
    }

    if (callbackData->pMessage != nullptr) {
        output << ": " << callbackData->pMessage;
    }

    output << '\n';
    return VK_FALSE;
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
    _supportsDeviceFault(false), _getDeviceFaultInfo(nullptr),
      _hasAsyncTransfer(false), _transferQueue(nullptr), _transferQueueFamily(UINT32_MAX) {

    vkb::InstanceBuilder instanceBuilder;
    auto instRet = instanceBuilder.set_app_name("ft_vox")
                       .request_validation_layers(true)
                       .set_debug_callback(vulkanDebugCallback)
                       .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                       .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
                       .add_validation_feature_enable(
                           VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT)
                       .add_validation_feature_enable(
                           VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
                       .add_validation_feature_enable(
                           VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT)
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

    vkb::PhysicalDevice vkbPhysicalDevice = physicalDeviceRet.value();
    _physicalDevice = vkbPhysicalDevice.physical_device;

    const bool supportsDeviceFaultExtension =
        hasDeviceExtension(_physicalDevice, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

    VkPhysicalDeviceFaultFeaturesEXT faultFeaturesQuery{};
    faultFeaturesQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;

    VkPhysicalDeviceFeatures2 faultFeaturesQuery2{};
    faultFeaturesQuery2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    faultFeaturesQuery2.pNext = supportsDeviceFaultExtension ? &faultFeaturesQuery : nullptr;

    if (supportsDeviceFaultExtension) {
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &faultFeaturesQuery2);
        _supportsDeviceFault = (faultFeaturesQuery.deviceFault == VK_TRUE);
    }

    VkPhysicalDeviceFaultFeaturesEXT faultFeaturesCreate{};
    faultFeaturesCreate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    faultFeaturesCreate.deviceFault = VK_TRUE;
    faultFeaturesCreate.deviceFaultVendorBinary = VK_FALSE;

    if (_supportsDeviceFault) {
        vkbPhysicalDevice.enable_extension_if_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }

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
            VK_EXT_MESH_SHADER_EXTENSION_NAME, // ADD THIS!
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME
        };

        if (_supportsDeviceFault) {
            deviceExts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        }

        // Build pNext chain
        if (_supportsDeviceFault) {
            faultFeaturesCreate.pNext = &meshShaderFeatures;
            features13.pNext = &faultFeaturesCreate;
        } else {
            features13.pNext = &meshShaderFeatures;
        }
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
            _meshShaderSupported = false;
        } else {
            _graphicsQueueFamily = graphicsFamily;
            _transferQueueFamily = transferFamily;
            vkGetDeviceQueue(_device, graphicsFamily, 0, &_graphicsQueue);
            vkGetDeviceQueue(_device, transferFamily, 0, &_transferQueue);
        }
    }

    if (!_meshShaderSupported) {
        vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
        if (_supportsDeviceFault) {
            faultFeaturesCreate.pNext = nullptr;
            deviceBuilder.add_pNext(&faultFeaturesCreate);
        }
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

    if (_supportsDeviceFault) {
        _getDeviceFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
            vkGetDeviceProcAddr(_device, "vkGetDeviceFaultInfoEXT"));
        if (_getDeviceFaultInfo == nullptr) {
            _supportsDeviceFault = false;
        }
    }

    // Setup VMA device memory callbacks for Tracy profiling
    VmaDeviceMemoryCallbacks vmaCallbacks{};
    vmaCallbacks.pfnAllocate = vmaAllocationCallback;
    vmaCallbacks.pfnFree = vmaDeallocationCallback;
    vmaCallbacks.pUserData = nullptr;

    VmaAllocatorCreateInfo allocatorInfo = {.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
                                                     VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT,
                                            .physicalDevice = _physicalDevice,
                                            .device = _device,
                                            .pDeviceMemoryCallbacks = &vmaCallbacks,
                                            .instance = _instance,
                                            .vulkanApiVersion = VK_API_VERSION_1_3};
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

void VulkanDevice::logDeviceFaultInfo(const char* context) const {
    std::cerr << "[Vulkan][DeviceLost] " << context << "\n";

    if (!_supportsDeviceFault || _device == nullptr || _getDeviceFaultInfo == nullptr) {
        std::cerr << "  VK_EXT_device_fault is not available on this device\n";
        return;
    }

    VkDeviceFaultCountsEXT faultCounts{};
    faultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

    VkResult countsResult = _getDeviceFaultInfo(_device, &faultCounts, nullptr);
    if (countsResult != VK_SUCCESS) {
        std::cerr << "  Failed to query device fault counts: " << countsResult << "\n";
        return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(faultCounts.vendorInfoCount);
    std::vector<uint8_t> vendorBinaryData(static_cast<size_t>(faultCounts.vendorBinarySize));

    VkDeviceFaultInfoEXT faultInfo{};
    faultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    faultInfo.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
    faultInfo.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
    faultInfo.pVendorBinaryData = vendorBinaryData.empty() ? nullptr : vendorBinaryData.data();

    VkResult infoResult = _getDeviceFaultInfo(_device, &faultCounts, &faultInfo);
    if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE) {
        std::cerr << "  Failed to query device fault info: " << infoResult << "\n";
        return;
    }

    if (faultInfo.description[0] != '\0') {
        std::cerr << "  Description: " << faultInfo.description << "\n";
    }

    std::cerr << "  Address infos: " << faultCounts.addressInfoCount << "\n";
    for (uint32_t i = 0; i < faultCounts.addressInfoCount; ++i) {
        const VkDeviceFaultAddressInfoEXT& addressInfo = addressInfos[i];
        std::cerr << "    [" << i << "] type=" << deviceFaultAddressTypeToString(addressInfo.addressType)
                  << " address=0x" << std::hex << addressInfo.reportedAddress << std::dec
                  << " precision=" << addressInfo.addressPrecision << "\n";
    }

    std::cerr << "  Vendor infos: " << faultCounts.vendorInfoCount << "\n";
    for (uint32_t i = 0; i < faultCounts.vendorInfoCount; ++i) {
        const VkDeviceFaultVendorInfoEXT& vendorInfo = vendorInfos[i];
        std::cerr << "    [" << i << "] code=" << vendorInfo.vendorFaultCode
                  << " data=" << vendorInfo.vendorFaultData;
        if (vendorInfo.description[0] != '\0') {
            std::cerr << " desc=" << vendorInfo.description;
        }
        std::cerr << "\n";
    }

    if (faultCounts.vendorBinarySize > 0) {
        std::cerr << "  Vendor binary size: " << faultCounts.vendorBinarySize << " bytes\n";
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

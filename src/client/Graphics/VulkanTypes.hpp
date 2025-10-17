#pragma once

#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

struct AllocatedBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
};

// holds the resources needed for a mesh
struct GPUMeshBuffers {
    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

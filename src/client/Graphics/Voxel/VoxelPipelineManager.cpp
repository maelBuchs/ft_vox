#include "VoxelPipelineManager.hpp"

#include <stdexcept>

#include <tracy/Tracy.hpp>

#include "../Core/VulkanDevice.hpp"
#include "../Memory/DescriptorAllocator.hpp"
#include "../Pipeline/ComputePipelineBuilder.hpp"
#include "../Pipeline/GraphicsPipelineBuilder.hpp"
#include "../Rendering/RenderContext.hpp"
#include "common/Types/RenderTypes.hpp"

VoxelPipelineManager::VoxelPipelineManager(VulkanDevice& device, RenderContext& context,
                                           DescriptorAllocatorGrowable& descriptorAllocator)
    : _device(device), _context(context), _descriptorAllocator(descriptorAllocator) {}

VoxelPipelineManager::~VoxelPipelineManager() {
    // Wait for GPU to finish before destroying resources
    vkDeviceWaitIdle(_device.getDevice());

    // Destroy pipelines
    _voxelPipeline.cleanup(_device);
    _voxelWireframePipeline.cleanup(_device);
    _meshShaderPipeline.cleanup(_device);
    _meshShaderWireframePipeline.cleanup(_device);
    _frustumCullPipeline.cleanup(_device);

    // Wait again to ensure pipelines are fully destroyed before layouts
    vkDeviceWaitIdle(_device.getDevice());

    // Destroy pipeline layouts
    if (_voxelPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _voxelPipelineLayout, nullptr);
        _voxelPipelineLayout = VK_NULL_HANDLE;
    }
    if (_meshShaderPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _meshShaderPipelineLayout, nullptr);
        _meshShaderPipelineLayout = VK_NULL_HANDLE;
    }
    if (_frustumCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.getDevice(), _frustumCullPipelineLayout, nullptr);
        _frustumCullPipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy descriptor set layouts
    if (_chunkSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _chunkSetLayout, nullptr);
    }
    if (_traditionalFragSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _traditionalFragSetLayout, nullptr);
    }
    if (_meshShaderSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _meshShaderSetLayout, nullptr);
    }
    if (_meshShaderFragSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _meshShaderFragSetLayout, nullptr);
    }
    if (_frustumCullSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.getDevice(), _frustumCullSetLayout, nullptr);
    }
}

void VoxelPipelineManager::initDescriptorLayouts() {
    ZoneScoped;

    // Chunk data layout (Set 0): SSBO + image sampler + atlas config UBO
    DescriptorLayoutBuilder chunkLayoutBuilder;
    chunkLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    chunkLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    chunkLayoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    _chunkSetLayout = chunkLayoutBuilder.build(
        _device.getDevice(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    // Traditional fragment layout (Set 1): texture atlas + atlas config
    DescriptorLayoutBuilder fragLayoutBuilder;
    fragLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    fragLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    _traditionalFragSetLayout =
        fragLayoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_FRAGMENT_BIT);

    // Compute culling layout: input chunks, frustum UBO, output indirect, output chunks
    DescriptorLayoutBuilder computeLayoutBuilder;
    computeLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Input chunks
    computeLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // Frustum data
    computeLayoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Output indirect
    computeLayoutBuilder.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Output chunk data
    _frustumCullSetLayout =
        computeLayoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_COMPUTE_BIT);

    // Mesh shader layouts (if supported, will be fully initialized in initMeshShaderPipelines)
    if (_device.supportsMeshShaders()) {
        DescriptorLayoutBuilder meshLayoutBuilder;
        meshLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // Camera UBO
        meshLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Chunk data
        meshLayoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // Index buffer
        _meshShaderSetLayout = meshLayoutBuilder.build(
            _device.getDevice(), VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT);

        DescriptorLayoutBuilder meshFragLayoutBuilder;
        meshFragLayoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        meshFragLayoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _meshShaderFragSetLayout =
            meshFragLayoutBuilder.build(_device.getDevice(), VK_SHADER_STAGE_FRAGMENT_BIT);
    }
}

void VoxelPipelineManager::initTraditionalPipelines() {
    ZoneScoped;

    VkShaderModule voxelFragShader =
        Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");
    VkShaderModule voxelVertexShader =
        Pipeline::loadShaderModule(_device, "shaders/voxel.vert.spv");

    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(glm::mat4) + sizeof(uint32_t)};

    VkDescriptorSetLayout setLayouts[] = {_chunkSetLayout, _traditionalFragSetLayout};

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 2,
        .pSetLayouts = setLayouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange};

    if (vkCreatePipelineLayout(_device.getDevice(), &pipelineLayoutInfo, nullptr,
                               &_voxelPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create voxel pipeline layout");
    }

    std::vector<VkVertexInputBindingDescription> noBindings{};
    std::vector<VkVertexInputAttributeDescription> noAttributes{};

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();

    // Create FILLED pipeline
    GraphicsPipelineBuilder pipelineBuilder;
    pipelineBuilder.setPipelineLayout(_voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(drawImage.format);
    pipelineBuilder.setDepthFormat(depthImage.format);
    pipelineBuilder.setVertexInputState(noBindings, noAttributes);

    VkPipeline voxelPipeline = pipelineBuilder.build(_device.getDevice());
    _voxelPipeline.init(voxelPipeline, _voxelPipelineLayout);

    // Create WIREFRAME pipeline
    pipelineBuilder.clear();
    pipelineBuilder.setPipelineLayout(_voxelPipelineLayout);
    pipelineBuilder.setShaders(voxelVertexShader, voxelFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_LINE);
    pipelineBuilder.setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    pipelineBuilder.setMultisamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS);
    pipelineBuilder.setColorAttachmentFormat(drawImage.format);
    pipelineBuilder.setDepthFormat(depthImage.format);
    pipelineBuilder.setVertexInputState(noBindings, noAttributes);

    VkPipeline voxelWireframePipeline = pipelineBuilder.build(_device.getDevice());
    _voxelWireframePipeline.init(voxelWireframePipeline, _voxelPipelineLayout);

    vkDestroyShaderModule(_device.getDevice(), voxelFragShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), voxelVertexShader, nullptr);
}

void VoxelPipelineManager::initComputeCullingPipeline() {
    ZoneScoped;

    // Use shared ComputeCullingPushConstants from RenderTypes.hpp
    VkPushConstantRange computePushRange{};
    computePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computePushRange.offset = 0;
    computePushRange.size = sizeof(ComputeCullingPushConstants);

    ComputePipelineBuilder computeBuilder;
    computeBuilder.setShader("shaders/frustum_cull.comp.spv");
    computeBuilder.setDescriptorSetLayout(_frustumCullSetLayout);
    computeBuilder.setPushConstantRange(computePushRange);

    ComputePipelineBuilder::BuildResult buildResult = computeBuilder.build(_device);

    _frustumCullPipelineLayout = buildResult.layout;
    _frustumCullPipeline.init(buildResult.pipeline, _frustumCullPipelineLayout);

    // Check for drawIndirectCount support
    VkPhysicalDeviceVulkan12Features supportedFeatures12{};
    supportedFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &supportedFeatures12;

    vkGetPhysicalDeviceFeatures2(_device.getPhysicalDevice(), &features2);
    _supportsDrawIndirectCount = (supportedFeatures12.drawIndirectCount == VK_TRUE);
}

bool VoxelPipelineManager::initMeshShaderPipelines() {
    ZoneScoped;

    if (!_device.supportsMeshShaders()) {
        _useMeshShaders = false;
        return false;
    }

    _vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        vkGetDeviceProcAddr(_device.getDevice(), "vkCmdDrawMeshTasksEXT"));

    if (_vkCmdDrawMeshTasksEXT == nullptr) {
        _useMeshShaders = false;
        return false;
    }

    _useMeshShaders = true;

    // Query mesh shader properties
    VkPhysicalDeviceMeshShaderPropertiesEXT meshProps{};
    meshProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    meshProps.pNext = nullptr;

    VkPhysicalDeviceProperties2 deviceProps{};
    deviceProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProps.pNext = &meshProps;
    vkGetPhysicalDeviceProperties2(_device.getPhysicalDevice(), &deviceProps);

    _maxMeshWorkgroupsPerTask = std::max(meshProps.maxMeshWorkGroupCount[0], 1u);

    // Use shared TaskShaderPushConstants from RenderTypes.hpp
    VkPushConstantRange pushRanges[2] = {};
    pushRanges[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRanges[0].offset = 0;
    pushRanges[0].size = sizeof(glm::mat4) + sizeof(uint32_t);

    pushRanges[1].stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT;
    pushRanges[1].offset = 68;
    pushRanges[1].size = sizeof(TaskShaderPushConstants);

    VkDescriptorSetLayout setLayouts[] = {_meshShaderSetLayout, _meshShaderFragSetLayout};

    VkPipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshLayoutInfo.setLayoutCount = 2;
    meshLayoutInfo.pSetLayouts = setLayouts;
    meshLayoutInfo.pushConstantRangeCount = 2;
    meshLayoutInfo.pPushConstantRanges = pushRanges;

    if (vkCreatePipelineLayout(_device.getDevice(), &meshLayoutInfo, nullptr,
                               &_meshShaderPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader pipeline layout");
    }

    VkShaderModule taskShader = Pipeline::loadShaderModule(_device, "shaders/voxel.task.spv");
    VkShaderModule meshShader = Pipeline::loadShaderModule(_device, "shaders/voxel.mesh.spv");
    VkShaderModule fragShader = Pipeline::loadShaderModule(_device, "shaders/voxel.frag.spv");

    const RenderContext::AllocatedImage& drawImage = _context.getDrawImage();
    const RenderContext::AllocatedImage& depthImage = _context.getDepthImage();

    VkPipelineShaderStageCreateInfo taskStageInfo{};
    taskStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    taskStageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    taskStageInfo.module = taskShader;
    taskStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo meshStageInfo{};
    meshStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshStageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshStageInfo.module = meshShader;
    meshStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragShader;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {taskStageInfo, meshStageInfo, fragStageInfo};

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &drawImage.format;
    renderingInfo.depthAttachmentFormat = depthImage.format;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 3;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = _meshShaderPipelineLayout;

    VkPipeline meshPipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                  &meshPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader pipeline");
    }
    _meshShaderPipeline.init(meshPipeline, _meshShaderPipelineLayout);

    // Create wireframe variant
    VkPipelineRasterizationStateCreateInfo wireframeRasterizer = rasterizer;
    wireframeRasterizer.polygonMode = VK_POLYGON_MODE_LINE;

    VkGraphicsPipelineCreateInfo wireframePipelineInfo = pipelineInfo;
    wireframePipelineInfo.pRasterizationState = &wireframeRasterizer;

    VkPipeline meshWireframePipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(_device.getDevice(), VK_NULL_HANDLE, 1, &wireframePipelineInfo,
                                  nullptr, &meshWireframePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader wireframe pipeline");
    }
    _meshShaderWireframePipeline.init(meshWireframePipeline, _meshShaderPipelineLayout);

    vkDestroyShaderModule(_device.getDevice(), taskShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), meshShader, nullptr);
    vkDestroyShaderModule(_device.getDevice(), fragShader, nullptr);

    return true;
}

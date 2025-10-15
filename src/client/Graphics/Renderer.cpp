#include "Renderer.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

#include "../Core/Window.hpp"
#include "VulkanDevice.hpp"
#include "VulkanSwapchain.hpp"

Renderer::Renderer(Window& window, VulkanDevice& device) : _window(window), _device(device) {
    try {
        _swapchain = std::make_unique<VulkanSwapchain>(window, device);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to create VulkanSwapchain: " << e.what() << "\n";
        throw;
    }
}

Renderer::~Renderer() {}

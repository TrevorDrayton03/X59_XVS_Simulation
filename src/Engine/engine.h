#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#ifndef VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS 1
#endif

#include <GLFW/glfw3.h>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <cstdint>
#include <vector>

class Engine
{
public:
    Engine() = default;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void initialize();
    void pollEvents() const;
    bool shouldClose() const;
    void drawFrame(const std::array<float, 4>& clearColor);

private:
    // Declared before all Vulkan members so GLFW shuts down AFTER Vulkan
    // handles (including the surface) have been destroyed.
    struct GlfwWindowOwner
    {
        GLFWwindow* handle = nullptr;
        bool initialized = false;
        ~GlfwWindowOwner();
    };

    GlfwWindowOwner window_;
    vk::raii::Context context_;
    vk::raii::Instance instance_{nullptr};
    vk::raii::PhysicalDevice physicalDevice_{nullptr};
    vk::raii::Device device_{nullptr};
    vk::raii::Queue queue_{nullptr};
    vk::raii::SurfaceKHR surface_{nullptr};
    vk::raii::SwapchainKHR swapchain_{nullptr};

    std::vector<vk::Image> swapchainImages_;
    std::vector<vk::raii::ImageView> swapchainImageViews_;
    vk::Format swapchainFormat_{vk::Format::eUndefined};
    vk::Extent2D swapchainExtent_{};

    vk::raii::CommandPool commandPool_{nullptr};
    vk::raii::CommandBuffers commandBuffers_{nullptr};
    std::vector<vk::raii::Fence> inFlightFences_;
    std::vector<vk::raii::Semaphore> imageAvailableSemaphores_;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;

    uint32_t queueFamilyIndex_ = UINT32_MAX;
    uint32_t framesInFlight_ = 2;
    uint32_t currentFrame_ = 0;

    void initWindow();
    void initVulkan();
    void createInstance();
    void createSurface();
    void choosePhysicalDevice();
    bool isDeviceSuitable(const vk::raii::PhysicalDevice& candidate) const;
    void createLogicalDevice();
    void createSwapchain();
    void createImageViews();
    void createCommandPool();
    void allocateCommandBuffers();
    void createSyncObjects();
};

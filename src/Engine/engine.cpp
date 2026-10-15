#include "engine.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
#ifdef NDEBUG
    constexpr bool enableValidationLayers = false;
#else
    constexpr bool enableValidationLayers = true;
#endif

    constexpr uint32_t WINDOW_WIDTH = 1200;
    constexpr uint32_t WINDOW_HEIGHT = 800;

    constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
    const std::vector<const char*> DEVICE_EXTENSIONS = {
        vk::KHRSwapchainExtensionName
    };
}

Engine::GlfwWindowOwner::~GlfwWindowOwner()
{
    if (handle)
    {
        glfwDestroyWindow(handle);
    }
    if (initialized)
    {
        glfwTerminate();
    }
}

Engine::~Engine()
{
    // RAII destroys Vulkan resources after this destructor body. The GLFW
    // window owner is the first member, so GLFW is destroyed last.
    if (*device_)
    {
        try
        {
            device_.waitIdle();
        }
        catch (...)
        {
            // Destructors must not throw.
        }
    }
}

void Engine::initialize()
{
    initWindow();
    initVulkan();
}

void Engine::pollEvents() const
{
    glfwPollEvents();
}

bool Engine::shouldClose() const
{
    return glfwWindowShouldClose(window_.handle) != 0;
}

void Engine::initWindow()
{
    if (glfwInit() != GLFW_TRUE)
    {
        throw std::runtime_error("glfwInit() failed");
    }
    window_.initialized = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    window_.handle = glfwCreateWindow(
        WINDOW_WIDTH, WINDOW_HEIGHT, "XVS Vulkan", nullptr, nullptr);
    if (!window_.handle)
    {
        throw std::runtime_error("glfwCreateWindow() failed");
    }
}

void Engine::createInstance()
{
    std::vector<const char*> enabledLayers;
    if (enableValidationLayers)
    {
        auto availableLayers = context_.enumerateInstanceLayerProperties();
        const auto found = std::ranges::any_of(availableLayers, [](const auto& layer)
        {
            return std::strcmp(layer.layerName, VALIDATION_LAYER) == 0;
        });
        if (!found)
        {
            throw std::runtime_error("Vulkan validation layer not available");
        }
        enabledLayers.push_back(VALIDATION_LAYER);
    }

    uint32_t extensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (!glfwExtensions || extensionCount == 0)
    {
        throw std::runtime_error("GLFW returned no required Vulkan extensions");
    }

    const std::vector<const char*> instanceExtensions(
        glfwExtensions, glfwExtensions + extensionCount);

    vk::ApplicationInfo appInfo{
        .pApplicationName = "XVS Vulkan",
        .applicationVersion = 1,
        .pEngineName = "XVS Engine",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_4,
    };

    vk::InstanceCreateInfo createInfo{
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    };

    instance_ = vk::raii::Instance(context_, createInfo);
}

void Engine::createSurface()
{
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(
            *instance_, window_.handle, nullptr, &rawSurface) != VK_SUCCESS)
    {
        throw std::runtime_error("glfwCreateWindowSurface() failed");
    }
    surface_ = vk::raii::SurfaceKHR(instance_, rawSurface);
}

bool Engine::isDeviceSuitable(const vk::raii::PhysicalDevice& candidate) const
{
    if (candidate.getProperties().apiVersion < VK_API_VERSION_1_4)
    {
        return false;
    }

    const auto families = candidate.getQueueFamilyProperties();
    bool hasGraphicsPresent = false;
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            candidate.getSurfaceSupportKHR(i, *surface_))
        {
            hasGraphicsPresent = true;
            break;
        }
    }
    if (!hasGraphicsPresent)
    {
        return false;
    }

    const auto extensions = candidate.enumerateDeviceExtensionProperties();
    for (const char* required : DEVICE_EXTENSIONS)
    {
        if (std::ranges::none_of(extensions, [required](const auto& item)
            { return std::strcmp(item.extensionName, required) == 0; }))
        {
            return false;
        }
    }

    const auto formats = candidate.getSurfaceFormatsKHR(*surface_);
    const auto presentModes = candidate.getSurfacePresentModesKHR(*surface_);
    if (formats.empty() || presentModes.empty())
    {
        return false;
    }

    const auto features = candidate.getFeatures2<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan13Features>();

    const auto& vulkan13 = features.get<vk::PhysicalDeviceVulkan13Features>();
    return vulkan13.dynamicRendering && vulkan13.synchronization2;
}

void Engine::choosePhysicalDevice()
{
    auto devices = instance_.enumeratePhysicalDevices();
    auto found = std::ranges::find_if(devices, [this](const auto& candidate)
    {
        return isDeviceSuitable(candidate);
    });

    if (found == devices.end())
    {
        throw std::runtime_error("No suitable Vulkan 1.4 GPU found");
    }
    physicalDevice_ = std::move(*found);
}

void Engine::createLogicalDevice()
{
    const auto families = physicalDevice_.getQueueFamilyProperties();
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            physicalDevice_.getSurfaceSupportKHR(i, *surface_))
        {
            queueFamilyIndex_ = i;
            break;
        }
    }
    if (queueFamilyIndex_ == UINT32_MAX)
    {
        throw std::runtime_error("No graphics + presentation queue family");
    }

    vk::StructureChain<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan13Features> features{
        {},
        {.synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE}
    };

    const float priority = 1.0f;
    vk::DeviceQueueCreateInfo queueInfo{
        .queueFamilyIndex = queueFamilyIndex_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    vk::DeviceCreateInfo deviceInfo{
        .pNext = &features.get<vk::PhysicalDeviceFeatures2>(),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<uint32_t>(DEVICE_EXTENSIONS.size()),
        .ppEnabledExtensionNames = DEVICE_EXTENSIONS.data(),
    };

    device_ = vk::raii::Device(physicalDevice_, deviceInfo);
    queue_ = vk::raii::Queue(device_, queueFamilyIndex_, 0);
}

void Engine::createSwapchain()
{
    const auto capabilities = physicalDevice_.getSurfaceCapabilitiesKHR(*surface_);
    const auto formats = physicalDevice_.getSurfaceFormatsKHR(*surface_);

    auto selected = std::ranges::find_if(formats, [](const auto& format)
    {
        return format.format == vk::Format::eB8G8R8A8Srgb &&
               format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
    });
    const vk::SurfaceFormatKHR surfaceFormat =
        selected != formats.end() ? *selected : formats.front();
    swapchainFormat_ = surfaceFormat.format;

    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        swapchainExtent_ = capabilities.currentExtent;
    }
    else
    {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_.handle, &width, &height);
        swapchainExtent_.width = std::clamp(
            static_cast<uint32_t>(std::max(0, width)),
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        swapchainExtent_.height = std::clamp(
            static_cast<uint32_t>(std::max(0, height)),
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = std::max(3u, capabilities.minImageCount);
    if (capabilities.maxImageCount != 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    vk::SwapchainCreateInfoKHR swapchainInfo{
        .surface = *surface_,
        .minImageCount = imageCount,
        .imageFormat = swapchainFormat_,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = swapchainExtent_,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = vk::PresentModeKHR::eFifo,
        .clipped = VK_TRUE,
    };

    swapchain_ = vk::raii::SwapchainKHR(device_, swapchainInfo);
    swapchainImages_ = swapchain_.getImages();
}

void Engine::createImageViews()
{
    swapchainImageViews_.reserve(swapchainImages_.size());
    for (const auto image : swapchainImages_)
    {
        vk::ImageViewCreateInfo viewInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = swapchainFormat_,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        swapchainImageViews_.emplace_back(device_, viewInfo);
    }
}

void Engine::createCommandPool()
{
    vk::CommandPoolCreateInfo poolInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = queueFamilyIndex_,
    };
    commandPool_ = vk::raii::CommandPool(device_, poolInfo);
}

void Engine::allocateCommandBuffers()
{
    vk::CommandBufferAllocateInfo allocateInfo{
        .commandPool = *commandPool_,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = framesInFlight_,
    };
    commandBuffers_ = vk::raii::CommandBuffers(device_, allocateInfo);
}

void Engine::createSyncObjects()
{
    inFlightFences_.reserve(framesInFlight_);
    imageAvailableSemaphores_.reserve(framesInFlight_);
    renderFinishedSemaphores_.reserve(swapchainImages_.size());

    // Render-finished semaphores belong to swapchain images, not flight slots.
    for (size_t i = 0; i < swapchainImages_.size(); ++i)
    {
        renderFinishedSemaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }

    for (uint32_t i = 0; i < framesInFlight_; ++i)
    {
        inFlightFences_.emplace_back(
            device_, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
        imageAvailableSemaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }
}

void Engine::initVulkan()
{
    createInstance();
    createSurface();
    choosePhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createImageViews();
    createCommandPool();
    allocateCommandBuffers();
    createSyncObjects();
}

void Engine::drawFrame(const std::array<float, 4>& clearColor)
{
    device_.waitForFences({*inFlightFences_[currentFrame_]}, VK_TRUE, UINT64_MAX);

    const auto [acquireResult, imageIndex] = swapchain_.acquireNextImage(
        UINT64_MAX, *imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE);
    (void)acquireResult;

    auto& commandBuffer = commandBuffers_[currentFrame_];
    commandBuffer.reset();
    commandBuffer.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier2 toColor{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchainImages_[imageIndex],
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vk::DependencyInfo toColorDependency{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toColor,
    };
    commandBuffer.pipelineBarrier2(toColorDependency);

    vk::RenderingAttachmentInfo colorAttachment{
        .imageView = *swapchainImageViews_[imageIndex],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue(vk::ClearColorValue{
            clearColor[0], clearColor[1], clearColor[2], clearColor[3]}),
    };

    vk::RenderingInfo renderInfo{
        .renderArea = {.offset = {0, 0}, .extent = swapchainExtent_},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
    };

    commandBuffer.beginRendering(renderInfo);
    // Future draw calls for the four XVS display regions go here.
    commandBuffer.endRendering();

    vk::ImageMemoryBarrier2 toPresent{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eNone,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchainImages_[imageIndex],
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vk::DependencyInfo presentDependency{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toPresent,
    };
    commandBuffer.pipelineBarrier2(presentDependency);
    commandBuffer.end();

    vk::CommandBufferSubmitInfo commandInfo{.commandBuffer = *commandBuffer};
    vk::SemaphoreSubmitInfo waitSemaphore{
        .semaphore = *imageAvailableSemaphores_[currentFrame_],
        .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    };
    const vk::Semaphore renderFinished = *renderFinishedSemaphores_[imageIndex];
    vk::SemaphoreSubmitInfo signalSemaphore{
        .semaphore = renderFinished,
        .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
    };
    vk::SubmitInfo2 submitInfo{
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitSemaphore,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalSemaphore,
    };

    device_.resetFences({*inFlightFences_[currentFrame_]});
    queue_.submit2(submitInfo, *inFlightFences_[currentFrame_]);

    const vk::SwapchainKHR swapchainHandle = *swapchain_;
    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderFinished,
        .swapchainCount = 1,
        .pSwapchains = &swapchainHandle,
        .pImageIndices = &imageIndex,
    };
    queue_.presentKHR(presentInfo);

    currentFrame_ = (currentFrame_ + 1) % framesInFlight_;
}

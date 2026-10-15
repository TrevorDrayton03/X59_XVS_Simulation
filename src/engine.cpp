#include "engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    VKAPI_ATTR vk::Bool32 VKAPI_CALL validationCallback(
        vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
        vk::DebugUtilsMessageTypeFlagsEXT,
        const vk::DebugUtilsMessengerCallbackDataEXT* callbackData,
        void*)
    {
        std::cerr << "Vulkan validation "
                  << ((severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
                      ? "error: " : "warning: ")
                  << callbackData->pMessage << '\n';
        return VK_FALSE;
    }

#ifndef XVS_SHADER_DIR
#define XVS_SHADER_DIR "shaders"
#endif

    std::filesystem::path shaderPath(const char* file)
    {
        return std::filesystem::path(XVS_SHADER_DIR) / file;
    }

    std::vector<uint32_t> readSpirv(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            throw std::runtime_error("Cannot open SPIR-V shader: " + path.string());
        }
        const auto byteCount = static_cast<std::streamsize>(file.tellg());
        if (byteCount <= 0 || (byteCount % sizeof(uint32_t)) != 0)
        {
            throw std::runtime_error("Invalid SPIR-V shader size: " + path.string());
        }
        std::vector<uint32_t> words(static_cast<size_t>(byteCount) / sizeof(uint32_t));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(words.data()),
                       static_cast<std::streamsize>(words.size() * sizeof(uint32_t))))
        {
            throw std::runtime_error("Failed reading shader: " + path.string());
        }
        return words;
    }

    vk::Rect2D toPixelRect(const RenderRegion& region, vk::Extent2D extent)
    {
        // Round *edges*, not independent widths, to reduce rounding gaps.
        const auto pixelX = [&](float v) -> uint32_t
        {
            return static_cast<uint32_t>(std::lround(
                std::clamp(v, 0.0f, 1.0f) * extent.width));
        };
        const auto pixelY = [&](float v) -> uint32_t
        {
            return static_cast<uint32_t>(std::lround(
                std::clamp(v, 0.0f, 1.0f) * extent.height));
        };
        const uint32_t left = pixelX(region.x);
        const uint32_t top = pixelY(region.y);
        const uint32_t right = std::max(left, pixelX(region.x + region.width));
        const uint32_t bottom = std::max(top, pixelY(region.y + region.height));
        return vk::Rect2D{
            .offset = {static_cast<int32_t>(left), static_cast<int32_t>(top)},
            .extent = {right - left, bottom - top}
        };
    }
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
            (void)device_.waitIdle();
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
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_.handle = glfwCreateWindow(
        WINDOW_WIDTH, WINDOW_HEIGHT, "XVS Vulkan", nullptr, nullptr);
    if (!window_.handle)
    {
        throw std::runtime_error("glfwCreateWindow() failed");
    }

    glfwSetWindowUserPointer(window_.handle, this);
    glfwSetFramebufferSizeCallback(window_.handle,
        [](GLFWwindow* window, int, int)
        {
            auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
            if (engine)
            {
                engine->framebufferResized_ = true;
            }
        });
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

    std::vector<const char*> instanceExtensions(
        glfwExtensions, glfwExtensions + extensionCount);
    if (enableValidationLayers)
    {
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const vk::DebugUtilsMessengerCreateInfoEXT debugInfo{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = validationCallback,
    };

    vk::ApplicationInfo appInfo{
        .pApplicationName = "XVS Vulkan",
        .applicationVersion = 1,
        .pEngineName = "XVS Engine",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_4,
    };

    vk::InstanceCreateInfo createInfo{
        .pNext = enableValidationLayers ? &debugInfo : nullptr,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    };

    instance_ = vk::raii::Instance(context_, createInfo);
    if (enableValidationLayers)
    {
        debugMessenger_ = vk::raii::DebugUtilsMessengerEXT(instance_, debugInfo);
        std::cout << "Vulkan validation enabled (warnings and errors go to stderr)\n";
    }
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

void Engine::createPipelineLayout()
{
    // A vec4 color for each display. A shader push constant is a small
    // piece of per-draw data; no descriptor sets or uniform buffers needed.
    vk::PushConstantRange colorRange{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 4 * sizeof(float),
    };
    vk::PipelineLayoutCreateInfo layoutInfo{
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &colorRange,
    };
    pipelineLayout_ = vk::raii::PipelineLayout(device_, layoutInfo);
}

void Engine::createGraphicsPipeline()
{
    const std::vector<uint32_t> vertexCode = readSpirv(shaderPath("region.vert.spv"));
    const std::vector<uint32_t> fragmentCode = readSpirv(shaderPath("region.frag.spv"));

    const vk::ShaderModuleCreateInfo vertexModuleInfo{
        .codeSize = vertexCode.size() * sizeof(uint32_t),
        .pCode = vertexCode.data(),
    };
    const vk::ShaderModuleCreateInfo fragmentModuleInfo{
        .codeSize = fragmentCode.size() * sizeof(uint32_t),
        .pCode = fragmentCode.data(),
    };
    vk::raii::ShaderModule vertexModule(device_, vertexModuleInfo);
    vk::raii::ShaderModule fragmentModule(device_, fragmentModuleInfo);

    const std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages{{
        {.stage = vk::ShaderStageFlagBits::eVertex,
         .module = *vertexModule, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment,
         .module = *fragmentModule, .pName = "main"},
    }};

    const vk::PipelineVertexInputStateCreateInfo vertexInput{};  // gl_VertexIndex only
    const vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vk::ColorComponentFlagBits::eR |
                          vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB |
                          vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo colorBlend{
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };
    const std::array<vk::DynamicState, 2> dynamicStates{
        vk::DynamicState::eViewport, vk::DynamicState::eScissor
    };
    const vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    // Required when the pipeline renders using vkCmdBeginRendering.
    const vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapchainFormat_,
    };
    const vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = &renderingInfo,
        .stageCount = static_cast<uint32_t>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = *pipelineLayout_,
        .renderPass = VK_NULL_HANDLE,
    };
    graphicsPipeline_ = vk::raii::Pipeline(device_, nullptr, pipelineInfo);
}

bool Engine::recreateSwapchain()
{
    // Minimized windows have a zero-area framebuffer; wait for restoration.
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_.handle, &width, &height);
    while (width == 0 || height == 0)
    {
        if (shouldClose())
        {
            return false;
        }
        glfwWaitEvents();
        glfwGetFramebufferSize(window_.handle, &width, &height);
    }

    // Simple and safe for Phase A; optimized in-flight recreation is not needed.
    (void)device_.waitIdle();

    graphicsPipeline_ = vk::raii::Pipeline(nullptr);
    renderFinishedSemaphores_.clear();
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    swapchain_ = vk::raii::SwapchainKHR(nullptr);

    createSwapchain();
    createImageViews();
    createGraphicsPipeline(); // Dynamic rendering format could have changed.
    for (size_t i = 0; i < swapchainImages_.size(); ++i)
    {
        renderFinishedSemaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }

    framebufferResized_ = false;
    return true;
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
    createPipelineLayout();
    createGraphicsPipeline();
    createCommandPool();
    allocateCommandBuffers();
    createSyncObjects();
}

void Engine::drawFrame(const std::array<float, 4>& clearColor,
                       std::span<const RenderRegion> regions)
{
    if (framebufferResized_ && !recreateSwapchain())
    {
        return;
    }

    (void)device_.waitForFences({*inFlightFences_[currentFrame_]}, VK_TRUE, UINT64_MAX);

    vk::Result acquireResult{};
    uint32_t imageIndex = 0;
    try
    {
        const auto acquired = swapchain_.acquireNextImage(
            UINT64_MAX, *imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE);
        acquireResult = acquired.result;
        imageIndex = acquired.value;
    }
    catch (const vk::OutOfDateKHRError&)
    {
        recreateSwapchain();
        return;
    }
    if (acquireResult == vk::Result::eErrorOutOfDateKHR)
    {
        recreateSwapchain();
        return;
    }

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

    // Phase A: one bound pipeline, four draws, each with its own
    // viewport, scissor, and push-constant color.
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *graphicsPipeline_);
    for (const RenderRegion& region : regions)
    {
        const vk::Rect2D rect = toPixelRect(region, swapchainExtent_);
        if (rect.extent.width == 0 || rect.extent.height == 0)
        {
            continue;
        }

        const vk::Viewport viewport{
            .x = static_cast<float>(rect.offset.x),
            .y = static_cast<float>(rect.offset.y),
            .width = static_cast<float>(rect.extent.width),
            .height = static_cast<float>(rect.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        commandBuffer.setViewport(0, viewport);
        commandBuffer.setScissor(0, rect);
        commandBuffer.pushConstants<float>(*pipelineLayout_,
                                           vk::ShaderStageFlagBits::eFragment,
                                           0, region.color);
        commandBuffer.draw(3, 1, 0, 0);
    }

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
    vk::Result presentResult{};
    try
    {
        presentResult = queue_.presentKHR(presentInfo);
    }
    catch (const vk::OutOfDateKHRError&)
    {
        presentResult = vk::Result::eErrorOutOfDateKHR;
    }

    currentFrame_ = (currentFrame_ + 1) % framesInFlight_;

    if (framebufferResized_ ||
        acquireResult == vk::Result::eSuboptimalKHR ||
        presentResult == vk::Result::eSuboptimalKHR ||
        presentResult == vk::Result::eErrorOutOfDateKHR)
    {
        recreateSwapchain();
    }
}

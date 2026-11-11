// Rhi.cpp — implementation of the Vulkan RHI.

#include "Rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

// Embedded SPIR-V byte arrays, generated from shaders/*.hlsl by CMake.
#include "triangle_vert_spv.h"
#include "triangle_frag_spv.h"

namespace pictor::render
{

namespace
{

struct Vertex
{
    float Position[2];
    float Color[3];
};

constexpr std::array<Vertex, 3> Vertices{{
    {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
}};

constexpr std::array<const char*, 1> DeviceExtensions{
    "VK_KHR_swapchain",
};

constexpr std::array<const char*, 1> ValidationLayers{
    "VK_LAYER_KHRONOS_validation",
};

vk::Bool32 debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT /*Severity*/,
    vk::DebugUtilsMessageTypeFlagsEXT /*Type*/,
    vk::DebugUtilsMessengerCallbackDataEXT const* Data,
    void* /*UserData*/)
{
    std::cerr << "[Vulkan validation] " << Data->pMessage << "\n";
    return vk::Bool32{0};  // VK_FALSE
}

}  // namespace

Rhi::Rhi(SDL_Window* window)
    : Window(window)
{
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createPipelineLayout();
    createPipeline();
    createFramebuffers();
    createCommandPool();
    createVertexBuffer();
    createCommandBuffers();
    createSyncObjects();
}

Rhi::~Rhi()
{
    // Flush the queue before the members are torn down (in reverse declaration
    // order) so no fence/semaphore/buffer is destroyed while still in use.
    if (*Device) {
        Device.waitIdle();
    }
}

void Rhi::createInstance()
{
    std::uint32_t ExtensionCount = 0;
    const char* const* SdlExtensions = SDL_Vulkan_GetInstanceExtensions(&ExtensionCount);
    if (!SdlExtensions) {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") +
                                 SDL_GetError());
    }
    std::vector<const char*> Extensions(SdlExtensions, SdlExtensions + ExtensionCount);

    bool IsValidationEnabled = false;
    {
        std::vector<vk::LayerProperties> Layers = Context.enumerateInstanceLayerProperties();
        for (const char* Name : ValidationLayers) {
            for (const vk::LayerProperties& Layer : Layers) {
                if (std::strcmp(Name, Layer.layerName) == 0) {
                    IsValidationEnabled = true;
                    break;
                }
            }
        }
    }
    if (IsValidationEnabled) {
        Extensions.push_back("VK_EXT_debug_utils");
    }

    vk::ApplicationInfo AppInfo("Hello Vulkan", 1, "No Engine", 1, vk::ApiVersion10);

    vk::InstanceCreateInfo CreateInfo({}, &AppInfo, ValidationLayers, Extensions);

    vk::DebugUtilsMessengerCreateInfoEXT DebugInfo;
    if (IsValidationEnabled) {
        DebugInfo.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                    vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
        DebugInfo.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
        DebugInfo.pfnUserCallback = debugCallback;
        CreateInfo.pNext = &DebugInfo;
    }

    Instance = vk::raii::Instance(Context, CreateInfo);

    if (IsValidationEnabled) {
        DebugMessenger = vk::raii::DebugUtilsMessengerEXT(Instance, DebugInfo);
    }
}

void Rhi::createSurface()
{
    VkSurfaceKHR VkSurface = nullptr;
    if (!SDL_Vulkan_CreateSurface(Window, static_cast<VkInstance>(*Instance), nullptr,
                                  &VkSurface)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") +
                                 SDL_GetError());
    }
    Surface = vk::raii::SurfaceKHR(Instance, VkSurface);
}

Rhi::QueueFamilyIndices Rhi::findQueueFamilies(
    const vk::raii::PhysicalDevice& physicalDevice) const
{
    QueueFamilyIndices Indices;

    std::vector<vk::QueueFamilyProperties> Families = physicalDevice.getQueueFamilyProperties();
    for (std::uint32_t Index = 0; Index < Families.size(); ++Index) {
        if (Families[Index].queueFlags & vk::QueueFlagBits::eGraphics) {
            Indices.GraphicsFamily = Index;
        }
        if (physicalDevice.getSurfaceSupportKHR(Index, *Surface)) {
            Indices.PresentFamily = Index;
        }
        if (Indices.isComplete()) {
            break;
        }
    }

    return Indices;
}

Rhi::SwapchainSupport Rhi::querySwapchainSupport(
    const vk::raii::PhysicalDevice& physicalDevice) const
{
    SwapchainSupport Details;
    Details.Capabilities = physicalDevice.getSurfaceCapabilitiesKHR(*Surface);
    Details.Formats = physicalDevice.getSurfaceFormatsKHR(*Surface);
    Details.PresentModes = physicalDevice.getSurfacePresentModesKHR(*Surface);
    return Details;
}

bool Rhi::isDeviceSuitable(const vk::raii::PhysicalDevice& physicalDevice) const
{
    QueueFamilyIndices Indices = findQueueFamilies(physicalDevice);
    if (!Indices.isComplete()) {
        return false;
    }

    bool AreExtensionsSupported = true;
    for (vk::ExtensionProperties Extension : physicalDevice.enumerateDeviceExtensionProperties()) {
        for (const char* Required : DeviceExtensions) {
            if (std::strcmp(Required, Extension.extensionName) == 0) {
                AreExtensionsSupported = true;
                break;
            }
        }
        if (AreExtensionsSupported) {
            break;
        }
    }

    SwapchainSupport Support = querySwapchainSupport(physicalDevice);
    return Indices.isComplete() && AreExtensionsSupported && !Support.Formats.empty() &&
           !Support.PresentModes.empty();
}

void Rhi::pickPhysicalDevice()
{
    vk::raii::PhysicalDevices Devices(Instance);
    for (auto& Device : Devices) {
        if (isDeviceSuitable(Device)) {
            PhysicalDevice = std::move(Device);
            break;
        }
    }
    if (!*PhysicalDevice) {
        throw std::runtime_error("No suitable GPU found (graphics + present + swapchain)");
    }

    vk::PhysicalDeviceProperties Props = PhysicalDevice.getProperties();
    std::cout << "[HelloVulkan] Using GPU: " << Props.deviceName.data() << " (Vulkan "
              << vk::apiVersionMajor(Props.apiVersion) << "."
              << vk::apiVersionMinor(Props.apiVersion) << ")\n";
}

void Rhi::createDevice()
{
    QueueFamilyIndices Indices = findQueueFamilies(PhysicalDevice);

    const float QueuePriority = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> QueueCreateInfos;
    std::vector<std::uint32_t> UniqueFamilies{Indices.GraphicsFamily};
    if (Indices.PresentFamily != Indices.GraphicsFamily) {
        UniqueFamilies.push_back(Indices.PresentFamily);
    }
    for (std::uint32_t Family : UniqueFamilies) {
        QueueCreateInfos.emplace_back(vk::DeviceQueueCreateFlags{}, Family, 1, &QueuePriority);
    }

    vk::PhysicalDeviceFeatures DeviceFeatures{};
    vk::DeviceCreateInfo CreateInfo({}, QueueCreateInfos, {}, DeviceExtensions,
                                    &DeviceFeatures);

    Device = vk::raii::Device(PhysicalDevice, CreateInfo);
    GraphicsQueue = Device.getQueue(Indices.GraphicsFamily, 0);
    PresentQueue = Device.getQueue(Indices.PresentFamily, 0);
}

vk::SurfaceFormatKHR Rhi::chooseSurfaceFormat(
    const std::vector<vk::SurfaceFormatKHR>& formats) const
{
    for (const vk::SurfaceFormatKHR& Format : formats) {
        if (Format.format == vk::Format::eB8G8R8A8Srgb &&
            Format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return Format;
        }
    }
    return formats.front();
}

vk::PresentModeKHR Rhi::choosePresentMode(const std::vector<vk::PresentModeKHR>& /*modes*/) const
{
    return vk::PresentModeKHR::eFifo;  // guaranteed to be available
}

vk::Extent2D Rhi::chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int Width = 0;
    int Height = 0;
    SDL_GetWindowSizeInPixels(Window, &Width, &Height);
    return vk::Extent2D{
        std::clamp(static_cast<std::uint32_t>(std::max(Width, 1)),
                   capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(static_cast<std::uint32_t>(std::max(Height, 1)),
                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

void Rhi::createSwapchain()
{
    SwapchainSupport Support = querySwapchainSupport(PhysicalDevice);
    vk::SurfaceFormatKHR SurfaceFormat = chooseSurfaceFormat(Support.Formats);
    vk::PresentModeKHR PresentMode = choosePresentMode(Support.PresentModes);
    vk::Extent2D Extent = chooseExtent(Support.Capabilities);

    std::uint32_t ImageCount = Support.Capabilities.minImageCount + 1;
    if (Support.Capabilities.maxImageCount > 0 &&
        ImageCount > Support.Capabilities.maxImageCount) {
        ImageCount = Support.Capabilities.maxImageCount;
    }

    QueueFamilyIndices Indices = findQueueFamilies(PhysicalDevice);
    std::vector<std::uint32_t> QueueFamilyIndices{Indices.GraphicsFamily, Indices.PresentFamily};

    vk::SwapchainCreateInfoKHR CreateInfo(
        {}, *Surface, ImageCount, SurfaceFormat.format, SurfaceFormat.colorSpace, Extent, 1,
        vk::ImageUsageFlagBits::eColorAttachment);
    if (Indices.GraphicsFamily != Indices.PresentFamily) {
        CreateInfo.imageSharingMode = vk::SharingMode::eConcurrent;
        CreateInfo.queueFamilyIndexCount = 2;
        CreateInfo.pQueueFamilyIndices = QueueFamilyIndices.data();
    }
    CreateInfo.preTransform = Support.Capabilities.currentTransform;
    CreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    CreateInfo.presentMode = PresentMode;
    CreateInfo.clipped = true;

    Swapchain = vk::raii::SwapchainKHR(Device, CreateInfo);

    SwapchainImageCount = static_cast<std::uint32_t>(Swapchain->getImages().size());
    SwapchainImageFormat = SurfaceFormat.format;
    SwapchainExtent = Extent;
}

void Rhi::createImageViews()
{
    SwapchainImageViews.clear();
    SwapchainImageViews.reserve(SwapchainImageCount);
    for (auto Image : Swapchain->getImages()) {
        vk::ImageViewCreateInfo CreateInfo(
            {}, Image, vk::ImageViewType::e2D, SwapchainImageFormat,
            vk::ComponentMapping(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1,
                                                              0, 1));
        SwapchainImageViews.emplace_back(Device, CreateInfo);
    }
}

void Rhi::createRenderPass()
{
    vk::AttachmentDescription ColorAttachment(
        {}, SwapchainImageFormat, vk::SampleCountFlagBits::e1, vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined,
        vk::ImageLayout::ePresentSrcKHR);

    vk::AttachmentReference ColorAttachmentRef(0, vk::ImageLayout::eColorAttachmentOptimal);

    vk::SubpassDescription Subpass({}, vk::PipelineBindPoint::eGraphics, {}, ColorAttachmentRef);

    vk::SubpassDependency Dependency(
        vk::SubpassExternal, 0, vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eNone,
        vk::AccessFlagBits::eColorAttachmentWrite);

    RenderPass = vk::raii::RenderPass(
        Device, vk::RenderPassCreateInfo({}, ColorAttachment, Subpass, Dependency));
}

void Rhi::createPipelineLayout()
{
    PipelineLayout = vk::raii::PipelineLayout(Device, vk::PipelineLayoutCreateInfo{});
}

void Rhi::createPipeline()
{
    vk::ShaderModuleCreateInfo VertInfo({}, sizeof(triangle_vert_spv),
                                        reinterpret_cast<const std::uint32_t*>(triangle_vert_spv));
    vk::ShaderModuleCreateInfo FragInfo({}, sizeof(triangle_frag_spv),
                                        reinterpret_cast<const std::uint32_t*>(triangle_frag_spv));
    vk::raii::ShaderModule VertModule(Device, VertInfo);
    vk::raii::ShaderModule FragModule(Device, FragInfo);

    vk::PipelineShaderStageCreateInfo VertStage({}, vk::ShaderStageFlagBits::eVertex, *VertModule,
                                                "main");
    vk::PipelineShaderStageCreateInfo FragStage({}, vk::ShaderStageFlagBits::eFragment, *FragModule,
                                                "main");
    std::array<vk::PipelineShaderStageCreateInfo, 2> Stages{VertStage, FragStage};

    vk::VertexInputBindingDescription Binding(0, sizeof(Vertex), vk::VertexInputRate::eVertex);
    std::array<vk::VertexInputAttributeDescription, 2> Attributes{
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat,
                                            offsetof(Vertex, Position)),
        vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat,
                                            offsetof(Vertex, Color)),
    };
    vk::PipelineVertexInputStateCreateInfo VertexInput({}, Binding, Attributes);

    vk::PipelineInputAssemblyStateCreateInfo InputAssembly(
        {}, vk::PrimitiveTopology::eTriangleList, false);

    vk::Viewport Viewport(0.0f, 0.0f, static_cast<float>(SwapchainExtent.width),
                          static_cast<float>(SwapchainExtent.height), 0.0f, 1.0f);
    vk::Rect2D Scissor(vk::Offset2D{0, 0}, SwapchainExtent);
    vk::PipelineViewportStateCreateInfo ViewportState({}, Viewport, Scissor);

    vk::PipelineRasterizationStateCreateInfo Rasterizer(
        {}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack,
        vk::FrontFace::eClockwise, false, 0.0f, 0.0f, 0.0f, 1.0f);

    vk::PipelineMultisampleStateCreateInfo Multisampling({}, vk::SampleCountFlagBits::e1);

    vk::PipelineColorBlendAttachmentState ColorBlendAttachment(
        false, vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendStateCreateInfo ColorBlending({}, false, vk::LogicOp::eCopy,
                                                        ColorBlendAttachment);

    vk::GraphicsPipelineCreateInfo PipelineInfo({}, Stages, &VertexInput, &InputAssembly, nullptr,
                                                &ViewportState, &Rasterizer, &Multisampling,
                                                nullptr, &ColorBlending, nullptr, *PipelineLayout,
                                                *RenderPass, 0, nullptr, -1);

    GraphicsPipeline = vk::raii::Pipeline(Device, nullptr, PipelineInfo);
}

void Rhi::createFramebuffers()
{
    SwapchainFramebuffers.clear();
    SwapchainFramebuffers.reserve(SwapchainImageViews.size());
    for (const vk::raii::ImageView& View : SwapchainImageViews) {
        vk::FramebufferCreateInfo CreateInfo(
            {}, *RenderPass, *View, SwapchainExtent.width, SwapchainExtent.height, 1);
        SwapchainFramebuffers.emplace_back(Device, CreateInfo);
    }
}

void Rhi::createCommandPool()
{
    CommandPool = vk::raii::CommandPool(
        Device, vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                          findQueueFamilies(PhysicalDevice).GraphicsFamily));
}

std::uint32_t Rhi::findMemoryType(std::uint32_t typeFilter,
                                  vk::MemoryPropertyFlags properties) const
{
    vk::PhysicalDeviceMemoryProperties MemProperties = PhysicalDevice.getMemoryProperties();
    for (std::uint32_t Index = 0; Index < MemProperties.memoryTypeCount; ++Index) {
        if ((typeFilter & (1u << Index)) &&
            (MemProperties.memoryTypes[Index].propertyFlags & properties) == properties) {
            return Index;
        }
    }
    throw std::runtime_error("Failed to find a suitable memory type");
}

void Rhi::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                       vk::MemoryPropertyFlags properties, vk::raii::Buffer& buffer,
                       vk::raii::DeviceMemory& memory)
{
    buffer = vk::raii::Buffer(Device, vk::BufferCreateInfo({}, size, usage));

    vk::MemoryRequirements Requirements = buffer.getMemoryRequirements();
    std::uint32_t MemoryTypeIndex = findMemoryType(Requirements.memoryTypeBits, properties);
    memory = vk::raii::DeviceMemory(Device, vk::MemoryAllocateInfo(Requirements.size,
                                                                   MemoryTypeIndex));
    buffer.bindMemory(*memory, 0);
}

void Rhi::createVertexBuffer()
{
    vk::DeviceSize BufferSize = sizeof(Vertex) * Vertices.size();

    vk::raii::Buffer StagingBuffer(nullptr);
    vk::raii::DeviceMemory StagingMemory(nullptr);
    createBuffer(BufferSize, vk::BufferUsageFlagBits::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eHostVisible |
                     vk::MemoryPropertyFlagBits::eHostCoherent,
                 StagingBuffer, StagingMemory);

    void* Data = StagingMemory.mapMemory(0, BufferSize);
    std::memcpy(Data, Vertices.data(), static_cast<size_t>(BufferSize));
    StagingMemory.unmapMemory();

    createBuffer(BufferSize,
                 vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, VertexBuffer, VertexBufferMemory);

    vk::raii::CommandBuffers TransferBuffers(
        Device, vk::CommandBufferAllocateInfo(*CommandPool, vk::CommandBufferLevel::ePrimary, 1));
    vk::raii::CommandBuffer& TransferBuffer = TransferBuffers.front();

    TransferBuffer.begin(
        vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    TransferBuffer.copyBuffer(*StagingBuffer, *VertexBuffer, vk::BufferCopy(0, 0, BufferSize));
    TransferBuffer.end();

    vk::CommandBuffer Raw = *TransferBuffer;
    vk::SubmitInfo SubmitInfo{};
    SubmitInfo.commandBufferCount = 1;
    SubmitInfo.pCommandBuffers = &Raw;
    GraphicsQueue.submit(SubmitInfo, nullptr);
    GraphicsQueue.waitIdle();
}

void Rhi::createCommandBuffers()
{
    CommandBuffers = Device.allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*CommandPool, vk::CommandBufferLevel::ePrimary,
                                      static_cast<std::uint32_t>(SwapchainFramebuffers.size())));

    for (size_t ImageIndex = 0; ImageIndex < CommandBuffers.size(); ++ImageIndex) {
        recordCommandBuffer(CommandBuffers[ImageIndex], ImageIndex);
    }
}

void Rhi::recordCommandBuffer(vk::raii::CommandBuffer& commandBuffer, size_t imageIndex)
{
    commandBuffer.begin(vk::CommandBufferBeginInfo{});

    vk::ClearValue ClearColor(
        vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}));
    vk::RenderPassBeginInfo RenderPassInfo(
        *RenderPass, *SwapchainFramebuffers[imageIndex],
        vk::Rect2D(vk::Offset2D{0, 0}, SwapchainExtent), ClearColor);

    commandBuffer.beginRenderPass(RenderPassInfo, vk::SubpassContents::eInline);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **GraphicsPipeline);
    commandBuffer.bindVertexBuffers(0, *VertexBuffer, {0});
    commandBuffer.draw(static_cast<std::uint32_t>(Vertices.size()), 1, 0, 0);
    commandBuffer.endRenderPass();

    commandBuffer.end();
}

void Rhi::createSyncObjects()
{
    ImageAvailableSemaphores.clear();
    RenderFinishedSemaphores.clear();
    InFlightFences.clear();

    for (std::uint32_t Index = 0; Index < MaxFramesInFlight; ++Index) {
        ImageAvailableSemaphores.emplace_back(Device, vk::SemaphoreCreateInfo{});
        InFlightFences.emplace_back(Device,
                                    vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
    }
    for (std::uint32_t Index = 0; Index < SwapchainImageCount; ++Index) {
        RenderFinishedSemaphores.emplace_back(Device, vk::SemaphoreCreateInfo{});
    }
}

void Rhi::drawFrame()
{
    vk::raii::Fence& Fence = InFlightFences[CurrentFrame];
    if (Device.waitForFences(*Fence, true, std::numeric_limits<std::uint64_t>::max()) !=
        vk::Result::eSuccess) {
        throw std::runtime_error("vkWaitForFences failed");
    }
    Device.resetFences(*Fence);

    auto [Result, ImageIndex] = Swapchain->acquireNextImage(
        std::numeric_limits<std::uint64_t>::max(), *ImageAvailableSemaphores[CurrentFrame],
        nullptr);
    if (Result == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapchain();
        return;
    }
    if (Result != vk::Result::eSuccess && Result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("vkAcquireNextImageKHR failed");
    }

    vk::PipelineStageFlags WaitStages = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo SubmitInfo(*ImageAvailableSemaphores[CurrentFrame], WaitStages,
                              *CommandBuffers[ImageIndex],
                              *RenderFinishedSemaphores[ImageIndex]);
    GraphicsQueue.submit(SubmitInfo, *Fence);

    vk::PresentInfoKHR PresentInfo(*RenderFinishedSemaphores[ImageIndex], **Swapchain,
                                   ImageIndex);
    Result = PresentQueue.presentKHR(PresentInfo);

    if (Result == vk::Result::eErrorOutOfDateKHR || Result == vk::Result::eSuboptimalKHR ||
        IsFramebufferResized) {
        IsFramebufferResized = false;
        recreateSwapchain();
        return;
    }
    if (Result != vk::Result::eSuccess) {
        throw std::runtime_error("vkQueuePresentKHR failed");
    }

    ++FrameCount;
    CurrentFrame = (CurrentFrame + 1) % MaxFramesInFlight;
}

void Rhi::recreateSwapchain()
{
    int Width = 0;
    int Height = 0;
    SDL_GetWindowSizeInPixels(Window, &Width, &Height);
    while (Width == 0 || Height == 0) {
        SDL_Event Event;
        SDL_WaitEvent(&Event);
        if (Event.type == SDL_EVENT_QUIT) {
            return;
        }
        SDL_GetWindowSizeInPixels(Window, &Width, &Height);
    }

    Device.waitIdle();

    Swapchain.reset();
    SwapchainImageViews.clear();
    SwapchainFramebuffers.clear();
    GraphicsPipeline.reset();
    CommandBuffers.clear();
    RenderFinishedSemaphores.clear();

    createSwapchain();
    createImageViews();
    createPipeline();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
    CurrentFrame = 0;
}

}  // namespace pictor::render

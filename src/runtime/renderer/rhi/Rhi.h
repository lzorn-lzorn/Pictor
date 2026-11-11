// Rhi.h — Render Hardware Interface.
//
// Owns the complete Vulkan 1.0 backend (instance, surface, device, swapchain,
// graphics pipeline and per-frame sync) and draws a single colored triangle.
// Vulkan is consumed through the vulkan-hpp headers (vulkan.hpp /
// vulkan_raii.hpp); all objects are managed by the vk::raii RAII wrappers.

#pragma once

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace pictor::render
{

class Rhi
{
public:
    explicit Rhi(SDL_Window* window);
    ~Rhi();

    Rhi(const Rhi&) = delete;
    Rhi& operator=(const Rhi&) = delete;

    void drawFrame();
    void setFramebufferResized(bool isResized) { IsFramebufferResized = isResized; }
    [[nodiscard]] std::uint64_t getFrameCount() const { return FrameCount; }

private:
    struct QueueFamilyIndices
    {
        std::uint32_t GraphicsFamily = ~0u;
        std::uint32_t PresentFamily = ~0u;
        [[nodiscard]] bool isComplete() const
        {
            return GraphicsFamily != ~0u && PresentFamily != ~0u;
        }
    };

    struct SwapchainSupport
    {
        vk::SurfaceCapabilitiesKHR Capabilities;
        std::vector<vk::SurfaceFormatKHR> Formats;
        std::vector<vk::PresentModeKHR> PresentModes;
    };

    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createPipelineLayout();
    void createPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createVertexBuffer();
    void createCommandBuffers();
    void createSyncObjects();
    void recordCommandBuffer(vk::raii::CommandBuffer& commandBuffer, size_t imageIndex);
    void recreateSwapchain();

    QueueFamilyIndices findQueueFamilies(const vk::raii::PhysicalDevice& physicalDevice) const;
    SwapchainSupport querySwapchainSupport(const vk::raii::PhysicalDevice& physicalDevice) const;
    bool isDeviceSuitable(const vk::raii::PhysicalDevice& physicalDevice) const;
    vk::SurfaceFormatKHR chooseSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& formats) const;
    vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& modes) const;
    vk::Extent2D chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities) const;
    std::uint32_t findMemoryType(std::uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
    void createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                      vk::MemoryPropertyFlags properties, vk::raii::Buffer& buffer,
                      vk::raii::DeviceMemory& memory);

    SDL_Window* Window = nullptr;

    // Declaration order is also destruction order (reverse), so children come
    // after their parents: the instance outlives surface/device, the device
    // outlives every device-level object below it. vk::raii wrappers delete
    // their default constructor, hence the `{nullptr}` initializers.
    vk::raii::Context Context;
    vk::raii::Instance Instance{nullptr};
    vk::raii::DebugUtilsMessengerEXT DebugMessenger{nullptr};
    vk::raii::SurfaceKHR Surface{nullptr};
    vk::raii::PhysicalDevice PhysicalDevice{nullptr};
    vk::raii::Device Device{nullptr};
    vk::raii::Queue GraphicsQueue{nullptr};
    vk::raii::Queue PresentQueue{nullptr};

    std::optional<vk::raii::SwapchainKHR> Swapchain;
    std::uint32_t SwapchainImageCount = 0;
    std::vector<vk::raii::ImageView> SwapchainImageViews;
    vk::Format SwapchainImageFormat = vk::Format::eUndefined;
    vk::Extent2D SwapchainExtent{};

    vk::raii::RenderPass RenderPass{nullptr};
    vk::raii::PipelineLayout PipelineLayout{nullptr};
    std::optional<vk::raii::Pipeline> GraphicsPipeline;
    std::vector<vk::raii::Framebuffer> SwapchainFramebuffers;

    vk::raii::CommandPool CommandPool{nullptr};
    vk::raii::CommandBuffers CommandBuffers{nullptr};

    vk::raii::Buffer VertexBuffer{nullptr};
    vk::raii::DeviceMemory VertexBufferMemory{nullptr};

    static constexpr std::uint32_t MaxFramesInFlight = 2;
    std::vector<vk::raii::Semaphore> ImageAvailableSemaphores;
    std::vector<vk::raii::Semaphore> RenderFinishedSemaphores;
    std::vector<vk::raii::Fence> InFlightFences;
    std::uint32_t CurrentFrame = 0;

    bool IsFramebufferResized = false;
    std::uint64_t FrameCount = 0;
};

}  // namespace pictor::render

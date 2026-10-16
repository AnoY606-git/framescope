// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "vulkan_dispatch.hpp"
#include "win_util.hpp"

#include <array>

namespace framescope::demo::detail {
inline constexpr unsigned kVulkanFrames = 2;

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override;
    void Initialize(HWND window, unsigned width, unsigned height, const Options& options) override;
    void Resize(unsigned width, unsigned height) override;
    PresentResult Render(const std::vector<Vertex>& vertices, unsigned drawCount) override;
    unsigned Width() const noexcept override { return extent_.width; }
    unsigned Height() const noexcept override { return extent_.height; }
    bool FullscreenAcquired() const noexcept override { return fullscreenAcquired_; }

private:
    struct Frame {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer vertices = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool coherent = false;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE; // Owned by the swapchain, not by us.
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        // Present-wait semaphores MUST be per image, not per CPU frame. Waiting
        // for a submit fence alone does not mean presentation consumed a wait.
        VkSemaphore presentReady = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };
    void CreateInstance();
    void SelectPhysicalDevice();
    void CreateDevice();
    void CreateFrames();
    void CreateVertexBuffer(Frame& frame);
    bool RebuildSwapchain();
    void CreatePipeline();
    VkSurfaceCapabilitiesKHR SurfaceCapabilities(bool& exclusivePolicy);
    std::vector<VkPresentModeKHR> PresentModes(bool exclusivePolicy);
    void DestroySwapchain() noexcept;
    void WaitFence(VkFence fence);
    void Record(Frame& frame, Image& image, std::uint32_t vertexCount, unsigned drawCount);

    // Loader outlives every Vulkan object and its dispatch pointers.
    SystemLibrary loader_;
    PFN_vkGetInstanceProcAddr getInstanceProc_ = nullptr;
    InstanceDispatch i_;
    DeviceDispatch d_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<Frame, kVulkanFrames> frames_{};
    std::vector<Image> images_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    HWND window_ = nullptr;
    Options options_;
    std::uint32_t graphicsFamily_ = 0, presentFamily_ = 0;
    std::uint32_t instanceVersion_ = VK_API_VERSION_1_0;
    unsigned requestedWidth_ = 0, requestedHeight_ = 0, frameIndex_ = 0;
    bool dirty_ = true;
    bool capabilities2_ = false, properties2_ = false;
    bool exclusiveExtension_ = false, fullscreenAcquired_ = false;
    bool warnedExclusive_ = false;
};
} // namespace framescope::demo::detail

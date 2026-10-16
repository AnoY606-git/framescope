// SPDX-License-Identifier: GPL-3.0-only
#include "vulkan_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

namespace framescope::demo::detail {
namespace {
bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}
} // namespace

VulkanRenderer::~VulkanRenderer() {
    if (device_ && d_.vkDeviceWaitIdle) d_.vkDeviceWaitIdle(device_);
    DestroySwapchain();
    for (auto& frame : frames_) {
        if (frame.mapped) d_.vkUnmapMemory(device_, frame.memory);
        if (frame.vertices) d_.vkDestroyBuffer(device_, frame.vertices, nullptr);
        if (frame.memory) d_.vkFreeMemory(device_, frame.memory, nullptr);
        if (frame.imageAvailable) d_.vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        if (frame.fence) d_.vkDestroyFence(device_, frame.fence, nullptr);
    }
    if (commandPool_) d_.vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_ && d_.vkDestroyDevice) d_.vkDestroyDevice(device_, nullptr);
    if (surface_ && i_.vkDestroySurfaceKHR) i_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_ && i_.vkDestroyInstance) i_.vkDestroyInstance(instance_, nullptr);
}

void VulkanRenderer::Initialize(HWND window, unsigned width, unsigned height, const Options& options) {
    window_ = window;
    options_ = options;
    requestedWidth_ = width; requestedHeight_ = height;
    loader_.Load(L"vulkan-1.dll");
    getInstanceProc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader_.Find("vkGetInstanceProcAddr"));
    if (!getInstanceProc_) throw std::runtime_error("System Vulkan loader has no vkGetInstanceProcAddr");
    CreateInstance();
    VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface.hinstance = GetModuleHandleW(nullptr);
    surface.hwnd = window_;
    CheckVk(i_.vkCreateWin32SurfaceKHR(instance_, &surface, nullptr, &surface_), "vkCreateWin32SurfaceKHR");
    SelectPhysicalDevice();
    CreateDevice();
    CreateFrames();
    // A window may be minimized/resized while device creation is in progress.
    // Defer swapchain creation to Render rather than treating that as an error.
    RebuildSwapchain();
}

void VulkanRenderer::CreateInstance() {
    const auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        getInstanceProc_(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        getInstanceProc_(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!enumerateExtensions || !createInstance) throw std::runtime_error("Incomplete system Vulkan loader");
    const auto extensions = EnumerateVk<VkExtensionProperties>([&](auto* count, auto* values) {
        return enumerateExtensions(nullptr, count, values);
    }, "Enumerate Vulkan instance extensions");
    if (!HasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !HasExtension(extensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        throw std::runtime_error("Vulkan runtime does not support Win32 presentation");
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        getInstanceProc_(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    std::uint32_t version = VK_API_VERSION_1_0;
    if (enumerateVersion) CheckVk(enumerateVersion(&version), "vkEnumerateInstanceVersion");
    instanceVersion_ = std::min(version, VK_API_VERSION_1_1);
    std::vector<const char*> enabled{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (options_.mode == WindowMode::Exclusive) {
        capabilities2_ = HasExtension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        properties2_ = HasExtension(extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (capabilities2_) enabled.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        if (properties2_) enabled.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "FrameScope Demo";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "FrameScope original triangle scene";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = instanceVersion_;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    create.ppEnabledExtensionNames = enabled.data();
    // No implicit HUD dependency and no layer is forced on here. The launcher
    // configures the real explicit layer through the environment before start.
    CheckVk(createInstance(&create, nullptr, &instance_), "vkCreateInstance");
    i_.Load(getInstanceProc_, instance_);
}

void VulkanRenderer::SelectPhysicalDevice() {
    const auto devices = EnumerateVk<VkPhysicalDevice>([&](auto* count, auto* values) {
        return i_.vkEnumeratePhysicalDevices(instance_, count, values);
    }, "vkEnumeratePhysicalDevices");
    int bestScore = -1;
    for (const auto physical : devices) {
        const auto extensions = EnumerateVk<VkExtensionProperties>([&](auto* count, auto* values) {
            return i_.vkEnumerateDeviceExtensionProperties(physical, nullptr, count, values);
        }, "Enumerate Vulkan device extensions");
        if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        std::uint32_t count = 0;
        i_.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        i_.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        families.resize(count);
        std::uint32_t graphics = UINT32_MAX, present = UINT32_MAX;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!families[index].queueCount) continue;
            VkBool32 supportsPresent = VK_FALSE;
            CheckVk(i_.vkGetPhysicalDeviceSurfaceSupportKHR(physical, index, surface_, &supportsPresent),
                    "vkGetPhysicalDeviceSurfaceSupportKHR");
            const bool supportsGraphics = (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (supportsGraphics && supportsPresent) { graphics = present = index; break; }
            if (supportsGraphics) graphics = index;
            if (supportsPresent) present = index;
        }
        if (graphics == UINT32_MAX || present == UINT32_MAX) continue;
        const auto formats = EnumerateVk<VkSurfaceFormatKHR>([&](auto* n, auto* values) {
            return i_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, n, values);
        }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        const auto modes = EnumerateVk<VkPresentModeKHR>([&](auto* n, auto* values) {
            return i_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, n, values);
        }, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        if (formats.empty() || modes.empty()) continue;
        VkPhysicalDeviceProperties properties{};
        i_.vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 300 :
                          properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 200 : 100;
        if (score <= bestScore) continue;
        bestScore = score;
        physical_ = physical;
        graphicsFamily_ = graphics; presentFamily_ = present;
        exclusiveExtension_ = options_.mode == WindowMode::Exclusive && capabilities2_ &&
            ((instanceVersion_ >= VK_API_VERSION_1_1 && properties.apiVersion >= VK_API_VERSION_1_1) || properties2_) &&
            i_.vkGetPhysicalDeviceSurfaceCapabilities2KHR &&
            i_.vkGetPhysicalDeviceSurfacePresentModes2EXT &&
            HasExtension(extensions, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
    }
    if (!physical_) throw std::runtime_error("No Vulkan graphics device supports this Win32 surface");
    i_.vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
    if (options_.mode == WindowMode::Exclusive && !exclusiveExtension_) {
        std::cerr << "Vulkan application-controlled exclusive fullscreen unsupported; continuing borderless, not exclusive.\n";
        warnedExclusive_ = true;
    }
}

void VulkanRenderer::CreateDevice() {
    // Check the advertised full-screen capability before enabling/acquiring it.
    if (exclusiveExtension_) {
        bool supported = false;
        SurfaceCapabilities(supported);
        if (!supported) exclusiveExtension_ = false;
    }
    if (options_.mode == WindowMode::Exclusive && !exclusiveExtension_ && !warnedExclusive_) {
        std::cerr << "Vulkan surface does not support application-controlled exclusive fullscreen; continuing borderless.\n";
        warnedExclusive_ = true;
    }
    const float priority = 1.0f;
    std::array<VkDeviceQueueCreateInfo, 2> queues{};
    queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = graphicsFamily_;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    queues[1] = queues[0];
    queues[1].queueFamilyIndex = presentFamily_;
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (exclusiveExtension_) extensions.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = graphicsFamily_ == presentFamily_ ? 1 : 2;
    create.pQueueCreateInfos = queues.data();
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    CheckVk(i_.vkCreateDevice(physical_, &create, nullptr, &device_), "vkCreateDevice");
    d_.Load(i_.vkGetDeviceProcAddr, device_);
    if (exclusiveExtension_ && (!d_.vkAcquireFullScreenExclusiveModeEXT || !d_.vkReleaseFullScreenExclusiveModeEXT))
        throw std::runtime_error("Vulkan exclusive extension advertised without its required functions");
    d_.vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    d_.vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
}

void VulkanRenderer::CreateFrames() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = graphicsFamily_;
    CheckVk(d_.vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool");
    std::array<VkCommandBuffer, kVulkanFrames> commands{};
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = kVulkanFrames;
    CheckVk(d_.vkAllocateCommandBuffers(device_, &allocate, commands.data()), "vkAllocateCommandBuffers");
    for (unsigned index = 0; index < kVulkanFrames; ++index) {
        auto& frame = frames_[index];
        frame.commands = commands[index];
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CheckVk(d_.vkCreateSemaphore(device_, &semaphore, nullptr, &frame.imageAvailable), "Create acquire semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CheckVk(d_.vkCreateFence(device_, &fence, nullptr, &frame.fence), "Create Vulkan frame fence");
        CreateVertexBuffer(frame);
    }
}

void VulkanRenderer::CreateVertexBuffer(Frame& frame) {
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = kMaxVertices * sizeof(Vertex);
    buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVk(d_.vkCreateBuffer(device_, &buffer, nullptr, &frame.vertices), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    d_.vkGetBufferMemoryRequirements(device_, frame.vertices, &requirements);
    std::uint32_t memoryType = UINT32_MAX;
    for (std::uint32_t index = 0; index < memoryProperties_.memoryTypeCount; ++index) {
        if (!(requirements.memoryTypeBits & (1u << index))) continue;
        const auto flags = memoryProperties_.memoryTypes[index].propertyFlags;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        memoryType = index;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
    if (memoryType == UINT32_MAX) throw std::runtime_error("No host-visible Vulkan vertex memory type");
    frame.coherent = (memoryProperties_.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType;
    CheckVk(d_.vkAllocateMemory(device_, &allocate, nullptr, &frame.memory), "vkAllocateMemory");
    CheckVk(d_.vkBindBufferMemory(device_, frame.vertices, frame.memory, 0), "vkBindBufferMemory");
    CheckVk(d_.vkMapMemory(device_, frame.memory, 0, VK_WHOLE_SIZE, 0, &frame.mapped), "vkMapMemory");
}

void VulkanRenderer::Resize(unsigned width, unsigned height) {
    requestedWidth_ = width; requestedHeight_ = height;
    dirty_ = true;
}

void VulkanRenderer::WaitFence(VkFence fence) {
    CheckVk(d_.vkWaitForFences(device_, 1, &fence, VK_TRUE, 30'000'000'000ull), "Vulkan frame fence wait (30 s)");
}

void VulkanRenderer::Record(Frame& frame, Image& image, std::uint32_t vertexCount, unsigned drawCount) {
    CheckVk(d_.vkResetCommandBuffer(frame.commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(d_.vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");
    VkClearValue clear{};
    std::copy(std::begin(kClearColor), std::end(kClearColor), clear.color.float32);
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = renderPass_;
    render.framebuffer = image.framebuffer;
    render.renderArea.extent = extent_;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    d_.vkCmdBeginRenderPass(frame.commands, &render, VK_SUBPASS_CONTENTS_INLINE);
    d_.vkCmdBindPipeline(frame.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkViewport viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    d_.vkCmdSetViewport(frame.commands, 0, 1, &viewport);
    d_.vkCmdSetScissor(frame.commands, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    d_.vkCmdBindVertexBuffers(frame.commands, 0, 1, &frame.vertices, &offset);
    for (unsigned draw = 0; draw < drawCount; ++draw) d_.vkCmdDraw(frame.commands, vertexCount, 1, 0, 0);
    d_.vkCmdEndRenderPass(frame.commands);
    CheckVk(d_.vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer");
}

PresentResult VulkanRenderer::Render(const std::vector<Vertex>& vertices, unsigned drawCount) {
    if (dirty_) {
        RebuildSwapchain();
        return PresentResult::Skipped; // Rebuild scene with the new actual extent.
    }
    if (vertices.size() > kMaxVertices) throw std::runtime_error("Scene exceeds vertex buffer capacity");
    auto& frame = frames_[frameIndex_];
    WaitFence(frame.fence);
    std::uint32_t index = 0;
    const auto acquired = d_.vkAcquireNextImageKHR(device_, swapchain_, 1'000'000'000ull,
        frame.imageAvailable, VK_NULL_HANDLE, &index);
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return PresentResult::Skipped;
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        if (acquired == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) fullscreenAcquired_ = false;
        dirty_ = true;
        return PresentResult::Skipped;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) CheckVk(acquired, "vkAcquireNextImageKHR");
    auto& image = images_.at(index);
    if (image.inFlight) WaitFence(image.inFlight);
    std::memcpy(frame.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    if (!frame.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = frame.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        CheckVk(d_.vkFlushMappedMemoryRanges(device_, 1, &range), "vkFlushMappedMemoryRanges");
    }
    Record(frame, image, static_cast<std::uint32_t>(vertices.size()), drawCount);
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &image.presentReady;
    // Reset only once recording succeeded. Out-of-date acquisition must never
    // leave an unsignaled frame fence that the next iteration would wait on.
    CheckVk(d_.vkResetFences(device_, 1, &frame.fence), "vkResetFences");
    CheckVk(d_.vkQueueSubmit(graphicsQueue_, 1, &submit, frame.fence), "vkQueueSubmit");
    image.inFlight = frame.fence;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image.presentReady;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    const auto result = d_.vkQueuePresentKHR(presentQueue_, &present);
    frameIndex_ = (frameIndex_ + 1) % kVulkanFrames;
    if (acquired == VK_SUBOPTIMAL_KHR || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
        dirty_ = true;
    if (result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        fullscreenAcquired_ = false;
        dirty_ = true;
        return PresentResult::Skipped;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return PresentResult::Skipped;
    if (result != VK_SUBOPTIMAL_KHR) CheckVk(result, "vkQueuePresentKHR");
    return PresentResult::Presented;
}
} // namespace framescope::demo::detail

namespace framescope::demo {
std::unique_ptr<Renderer> MakeVulkanRenderer() { return std::make_unique<detail::VulkanRenderer>(); }
} // namespace framescope::demo

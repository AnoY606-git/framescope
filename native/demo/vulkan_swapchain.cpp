// SPDX-License-Identifier: GPL-3.0-only
#include "vulkan_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>

namespace framescope::demo::detail {
namespace {
using std::uint32_t;
#include "shaders/scene_vert_spv.hpp"
#include "shaders/scene_frag_spv.hpp"

struct ShaderModule {
    DeviceDispatch& dispatch;
    VkDevice device;
    VkShaderModule module = VK_NULL_HANDLE;
    ~ShaderModule() { if (module) dispatch.vkDestroyShaderModule(device, module, nullptr); }
};

VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED &&
        formats[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const VkFormat preferred : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        for (const auto& format : formats)
            if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
    }
    throw std::runtime_error("Vulkan surface offers no supported SDR RGBA/BGRA format");
}
} // namespace

VkSurfaceCapabilitiesKHR VulkanRenderer::SurfaceCapabilities(bool& exclusivePolicy) {
    exclusivePolicy = false;
    if (exclusiveExtension_) {
        VkSurfaceFullScreenExclusiveWin32InfoEXT win32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        win32.hmonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        VkSurfaceFullScreenExclusiveInfoEXT policy{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        policy.pNext = &win32;
        policy.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        VkPhysicalDeviceSurfaceInfo2KHR surface{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        surface.pNext = &policy;
        surface.surface = surface_;
        VkSurfaceCapabilitiesFullScreenExclusiveEXT exclusive{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT};
        VkSurfaceCapabilities2KHR capabilities{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        capabilities.pNext = &exclusive;
        CheckVk(i_.vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical_, &surface, &capabilities),
                "Query Vulkan application-controlled fullscreen capability");
        if (exclusive.fullScreenExclusiveSupported) {
            exclusivePolicy = true;
            return capabilities.surfaceCapabilities;
        }
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    CheckVk(i_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return capabilities;
}

std::vector<VkPresentModeKHR> VulkanRenderer::PresentModes(bool exclusivePolicy) {
    if (exclusivePolicy) {
        VkSurfaceFullScreenExclusiveWin32InfoEXT win32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        win32.hmonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        VkSurfaceFullScreenExclusiveInfoEXT policy{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        policy.pNext = &win32;
        policy.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        VkPhysicalDeviceSurfaceInfo2KHR surface{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        surface.pNext = &policy;
        surface.surface = surface_;
        return EnumerateVk<VkPresentModeKHR>([&](auto* count, auto* modes) {
            return i_.vkGetPhysicalDeviceSurfacePresentModes2EXT(physical_, &surface, count, modes);
        }, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
    }
    return EnumerateVk<VkPresentModeKHR>([&](auto* count, auto* modes) {
        return i_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, count, modes);
    }, "vkGetPhysicalDeviceSurfacePresentModesKHR");
}

bool VulkanRenderer::RebuildSwapchain() {
    RECT client{};
    if (!GetClientRect(window_, &client)) throw std::runtime_error("GetClientRect failed during Vulkan resize");
    if (IsIconic(window_) || client.right <= 0 || client.bottom <= 0) return false;
    requestedWidth_ = static_cast<unsigned>(client.right);
    requestedHeight_ = static_cast<unsigned>(client.bottom);
    bool exclusivePolicy = false;
    const auto capabilities = SurfaceCapabilities(exclusivePolicy);
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        extent.width = std::clamp(requestedWidth_, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(requestedHeight_, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        throw std::runtime_error("Vulkan surface cannot be used as a color attachment");
    const auto formats = EnumerateVk<VkSurfaceFormatKHR>([&](auto* count, auto* values) {
        return i_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, count, values);
    }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    const auto format = ChooseFormat(formats);
    const auto modes = PresentModes(exclusivePolicy);
    const auto hasMode = [&](VkPresentModeKHR mode) {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!options_.vsync) {
        if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR)) presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (!hasMode(presentMode)) throw std::runtime_error("Vulkan surface offers no selected presentation mode");
    std::uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
    if (capabilities.maxImageCount) imageCount = std::min(imageCount, capabilities.maxImageCount);
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (capabilities.supportedCompositeAlpha & candidate) { composite = candidate; break; }
    }
    CheckVk(d_.vkDeviceWaitIdle(device_), "Wait for Vulkan swapchain resize");
    DestroySwapchain();
    VkSurfaceFullScreenExclusiveWin32InfoEXT win32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    win32.hmonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    VkSurfaceFullScreenExclusiveInfoEXT policy{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    policy.pNext = &win32;
    policy.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    if (exclusivePolicy) create.pNext = &policy;
    create.surface = surface_;
    create.minImageCount = imageCount;
    create.imageFormat = format.format;
    create.imageColorSpace = format.colorSpace;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const std::uint32_t families[] = {graphicsFamily_, presentFamily_};
    if (graphicsFamily_ != presentFamily_) {
        create.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = 2;
        create.pQueueFamilyIndices = families;
    } else {
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = composite;
    create.presentMode = presentMode;
    create.clipped = VK_TRUE;
    const auto created = d_.vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_);
    if (created == VK_ERROR_OUT_OF_DATE_KHR) { dirty_ = true; return false; }
    if (exclusivePolicy && (created == VK_ERROR_INITIALIZATION_FAILED ||
                            created == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) {
        std::cerr << "Vulkan exclusive swapchain unavailable; retrying without exclusive policy.\n";
        exclusiveExtension_ = false;
        return RebuildSwapchain();
    }
    CheckVk(created, "vkCreateSwapchainKHR");
    const auto handles = EnumerateVk<VkImage>([&](auto* count, auto* values) {
        return d_.vkGetSwapchainImagesKHR(device_, swapchain_, count, values);
    }, "vkGetSwapchainImagesKHR");
    if (handles.empty()) throw std::runtime_error("Vulkan swapchain contains no images");
    images_.resize(handles.size());
    extent_ = extent; format_ = format.format;
    for (std::size_t index = 0; index < handles.size(); ++index) {
        auto& image = images_[index];
        image.image = handles[index];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        CheckVk(d_.vkCreateImageView(device_, &view, nullptr, &image.view), "vkCreateImageView");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CheckVk(d_.vkCreateSemaphore(device_, &semaphore, nullptr, &image.presentReady), "Create per-image present semaphore");
    }
    CreatePipeline();
    for (auto& image : images_) {
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = renderPass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &image.view;
        framebuffer.width = extent_.width;
        framebuffer.height = extent_.height;
        framebuffer.layers = 1;
        CheckVk(d_.vkCreateFramebuffer(device_, &framebuffer, nullptr, &image.framebuffer), "vkCreateFramebuffer");
    }
    if (exclusivePolicy) {
        const auto acquired = d_.vkAcquireFullScreenExclusiveModeEXT(device_, swapchain_);
        if (acquired == VK_ERROR_DEVICE_LOST) CheckVk(acquired, "Acquire Vulkan exclusive fullscreen");
        fullscreenAcquired_ = acquired == VK_SUCCESS;
        if (!fullscreenAcquired_)
            std::cerr << "Vulkan exclusive access not acquired (VkResult " << static_cast<int>(acquired)
                      << "); continuing without exclusive access.\n";
    } else if (options_.mode == WindowMode::Exclusive && !warnedExclusive_) {
        std::cerr << "Vulkan exclusive fullscreen unsupported for this surface; continuing borderless.\n";
        warnedExclusive_ = true;
    }
    dirty_ = false;
    return true;
}

void VulkanRenderer::DestroySwapchain() noexcept {
    if (fullscreenAcquired_ && swapchain_ && d_.vkReleaseFullScreenExclusiveModeEXT)
        d_.vkReleaseFullScreenExclusiveModeEXT(device_, swapchain_);
    fullscreenAcquired_ = false;
    for (auto& image : images_) {
        if (image.framebuffer) d_.vkDestroyFramebuffer(device_, image.framebuffer, nullptr);
        if (image.view) d_.vkDestroyImageView(device_, image.view, nullptr);
        if (image.presentReady) d_.vkDestroySemaphore(device_, image.presentReady, nullptr);
    }
    images_.clear();
    if (pipeline_) d_.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) d_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (renderPass_) d_.vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (swapchain_) d_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    pipeline_ = VK_NULL_HANDLE; pipelineLayout_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE; swapchain_ = VK_NULL_HANDLE;
}

void VulkanRenderer::CreatePipeline() {
    VkAttachmentDescription attachment{};
    attachment.format = format_;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo renderPass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPass.attachmentCount = 1;
    renderPass.pAttachments = &attachment;
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    renderPass.dependencyCount = 1;
    renderPass.pDependencies = &dependency;
    CheckVk(d_.vkCreateRenderPass(device_, &renderPass, nullptr, &renderPass_), "vkCreateRenderPass");

    ShaderModule vertex{d_, device_}, fragment{d_, device_};
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = sizeof(kSceneVertexSpv);
    shader.pCode = kSceneVertexSpv;
    CheckVk(d_.vkCreateShaderModule(device_, &shader, nullptr, &vertex.module), "Create original Vulkan vertex shader");
    shader.codeSize = sizeof(kSceneFragmentSpv);
    shader.pCode = kSceneFragmentSpv;
    CheckVk(d_.vkCreateShaderModule(device_, &shader, nullptr, &fragment.module), "Create original Vulkan fragment shader");
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CheckVk(d_.vkCreatePipelineLayout(device_, &layout, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.module;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.module;
    stages[1].pName = "main";
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, r)},
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 2;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &rasterizer;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = pipelineLayout_;
    pipeline.renderPass = renderPass_;
    CheckVk(d_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
            "vkCreateGraphicsPipelines");
}
} // namespace framescope::demo::detail

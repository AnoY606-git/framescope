// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "demo.hpp"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace framescope::demo::detail {
inline void CheckVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " +
                                 std::to_string(static_cast<int>(result)) + ')');
}

template <class T, class F>
std::vector<T> EnumerateVk(F&& enumerate, const char* operation) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::uint32_t count = 0;
        CheckVk(enumerate(&count, static_cast<T*>(nullptr)), operation);
        if (!count) return {};
        std::vector<T> values(count);
        const auto result = enumerate(&count, values.data());
        if (result == VK_INCOMPLETE) continue;
        CheckVk(result, operation);
        values.resize(count);
        return values;
    }
    throw std::runtime_error(std::string(operation) + " did not stabilize");
}

#define FRAMESCOPE_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr)

struct InstanceDispatch {
#define FRAMESCOPE_DECLARE_VK(name) PFN_##name name = nullptr;
    FRAMESCOPE_INSTANCE_FUNCTIONS(FRAMESCOPE_DECLARE_VK)
#undef FRAMESCOPE_DECLARE_VK
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR vkGetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT vkGetPhysicalDeviceSurfacePresentModes2EXT = nullptr;
    void Load(PFN_vkGetInstanceProcAddr get, VkInstance instance) {
#define FRAMESCOPE_LOAD_VK(name) \
        name = reinterpret_cast<PFN_##name>(get(instance, #name)); \
        if (!name) throw std::runtime_error("Missing Vulkan instance function: " #name);
        FRAMESCOPE_INSTANCE_FUNCTIONS(FRAMESCOPE_LOAD_VK)
#undef FRAMESCOPE_LOAD_VK
        vkGetPhysicalDeviceSurfaceCapabilities2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            get(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
        vkGetPhysicalDeviceSurfacePresentModes2EXT = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT>(
            get(instance, "vkGetPhysicalDeviceSurfacePresentModes2EXT"));
    }
};
#undef FRAMESCOPE_INSTANCE_FUNCTIONS

#define FRAMESCOPE_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindVertexBuffers) X(vkCmdDraw) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges)

struct DeviceDispatch {
#define FRAMESCOPE_DECLARE_VK(name) PFN_##name name = nullptr;
    FRAMESCOPE_DEVICE_FUNCTIONS(FRAMESCOPE_DECLARE_VK)
#undef FRAMESCOPE_DECLARE_VK
    PFN_vkAcquireFullScreenExclusiveModeEXT vkAcquireFullScreenExclusiveModeEXT = nullptr;
    PFN_vkReleaseFullScreenExclusiveModeEXT vkReleaseFullScreenExclusiveModeEXT = nullptr;
    void Load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
#define FRAMESCOPE_LOAD_VK(name) \
        name = reinterpret_cast<PFN_##name>(get(device, #name)); \
        if (!name) throw std::runtime_error("Missing Vulkan device function: " #name);
        FRAMESCOPE_DEVICE_FUNCTIONS(FRAMESCOPE_LOAD_VK)
#undef FRAMESCOPE_LOAD_VK
        vkAcquireFullScreenExclusiveModeEXT = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
            get(device, "vkAcquireFullScreenExclusiveModeEXT"));
        vkReleaseFullScreenExclusiveModeEXT = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
            get(device, "vkReleaseFullScreenExclusiveModeEXT"));
    }
};
#undef FRAMESCOPE_DEVICE_FUNCTIONS
} // namespace framescope::demo::detail

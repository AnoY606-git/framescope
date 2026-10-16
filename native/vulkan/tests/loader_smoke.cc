// SPDX-License-Identifier: GPL-3.0-only
// Real system-loader bootstrap only: no window, swapchain, HUD, game or global setup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) {
        std::printf("SKIP: system Vulkan loader unavailable (%lu).\n", GetLastError());
        return 77;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!gipa) return 1;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
    uint32_t count = 0;
    if (!enumerate || enumerate(&count, nullptr) != VK_SUCCESS) return 2;
    std::vector<VkLayerProperties> layers(count);
    if (enumerate(&count, layers.data()) != VK_SUCCESS) return 3;
    bool found = false;
    for (const auto& layer : layers)
        if (!std::strcmp(layer.layerName, "VK_LAYER_FRAMESCOPE_hud")) found = true;
    if (!found) { std::printf("FAIL: explicit layer not discovered.\n"); return 4; }
    const char* enabled = "VK_LAYER_FRAMESCOPE_hud";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "FrameScope Vulkan loader smoke (no rendering)";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    create.enabledLayerCount = 1;
    create.ppEnabledLayerNames = &enabled;
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create_instance(&create, nullptr, &instance);
    std::printf("vkCreateInstance: %d\n", result);
    if (result != VK_SUCCESS) return 5;
    auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto physicals = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    if (!destroy_instance || !physicals || !properties || !families || !create_device) return 6;
    count = 0;
    result = physicals(instance, &count, nullptr);
    if (result != VK_SUCCESS || !count) { destroy_instance(instance, nullptr); return 7; }
    std::vector<VkPhysicalDevice> devices(count);
    result = physicals(instance, &count, devices.data());
    if (result != VK_SUCCESS) { destroy_instance(instance, nullptr); return 8; }
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t family_index = 0;
    for (VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties props{};
        properties(physical, &props);
        std::printf("Adapter: %s, API %u.%u.%u\n", props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
        uint32_t family_count = 0;
        families(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(family_count);
        families(physical, &family_count, queues.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (!selected || props.vendorID == 0x8086)) {
                selected = physical;
                family_index = i;
                break;
            }
        }
    }
    if (!selected) { destroy_instance(instance, nullptr); return 9; }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family_index;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    result = create_device(selected, &device_info, nullptr, &device);
    std::printf("vkCreateDevice: %d\n", result);
    if (result != VK_SUCCESS) { destroy_instance(instance, nullptr); return 10; }
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    if (!gdpa) return 11;
    auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(device, "vkGetDeviceQueue"));
    auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"));
    if (!get_queue || !destroy_device) return 12;
    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, family_index, 0, &queue);
    if (!queue) return 13;
    destroy_device(device, nullptr);
    destroy_instance(instance, nullptr);
    FreeLibrary(loader);
    std::printf("PASS: real Vulkan loader + explicit layer instance/device/queue lifecycle. No HUD rendering tested.\n");
    return 0;
}

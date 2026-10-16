// SPDX-License-Identifier: GPL-3.0-only
// Standalone mock tests. .cc intentionally isn't included by the parent's *.cpp
// DLL source glob. No graphics driver, game process, registry or global env needed.
#include "../layer.cpp"

#include <cstdio>
#include <cstdlib>

namespace {

unsigned g_checks = 0;

void Require(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct MockDispatchable { void* dispatch; };
int g_instance_table;
int g_device_table;
MockDispatchable g_mock_instance{&g_instance_table};
MockDispatchable g_mock_physical{&g_instance_table};
MockDispatchable g_mock_device{&g_device_table};
MockDispatchable g_mock_queue{&g_device_table};
uint32_t g_creates = 0;
uint32_t g_destroys = 0;
uint32_t g_submits = 0;
uint32_t g_present_calls = 0;
uint32_t g_queue_idle = 0;
bool g_instance_link_advanced = false;
bool g_device_link_advanced = false;
VkResult g_create_swap_result = VK_SUCCESS;
VkResult g_present_result = VK_SUCCESS;
const VkPresentInfoKHR* g_last_present = nullptr;
uint64_t g_swap_id = 100;

template <typename Handle>
Handle NonDispatchable(uint64_t id) { return reinterpret_cast<Handle>(static_cast<uintptr_t>(id)); }

VKAPI_ATTR VkResult VKAPI_CALL MockCreateInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks*, VkInstance* instance) {
    g_instance_link_advanced = InstanceChain(info->pNext, VK_LAYER_LINK_INFO)->u.pLayerInfo == nullptr;
    *instance = reinterpret_cast<VkInstance>(&g_mock_instance);
    ++g_creates;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL MockDestroyInstance(VkInstance, const VkAllocationCallbacks*) { ++g_destroys; }

VKAPI_ATTR VkResult VKAPI_CALL MockEnumeratePhysical(VkInstance, uint32_t* count, VkPhysicalDevice* devices) {
    if (devices && *count) devices[0] = reinterpret_cast<VkPhysicalDevice>(&g_mock_physical);
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL MockCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks*, VkDevice* device) {
    g_device_link_advanced = DeviceChain(info->pNext, VK_LAYER_LINK_INFO)->u.pLayerInfo == nullptr;
    *device = reinterpret_cast<VkDevice>(&g_mock_device);
    ++g_creates;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL MockDestroyDevice(VkDevice, const VkAllocationCallbacks*) { ++g_destroys; }

VKAPI_ATTR void VKAPI_CALL MockGetQueue(VkDevice, uint32_t, uint32_t, VkQueue* queue) {
    *queue = reinterpret_cast<VkQueue>(&g_mock_queue);
}

VKAPI_ATTR VkResult VKAPI_CALL MockSetLoaderData(VkDevice, void*) { return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL MockCreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR*,
    const VkAllocationCallbacks*, VkSwapchainKHR* swapchain) {
    if (g_create_swap_result == VK_SUCCESS) *swapchain = NonDispatchable<VkSwapchainKHR>(++g_swap_id);
    return g_create_swap_result;
}

VKAPI_ATTR void VKAPI_CALL MockDestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL MockImages(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* images) {
    if (images && *count >= 2) {
        images[0] = NonDispatchable<VkImage>(501);
        images[1] = NonDispatchable<VkImage>(502);
    }
    *count = 2;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL MockAcquire(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence,
                                          uint32_t* index) {
    *index = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL MockPresent(VkQueue, const VkPresentInfoKHR* info) {
    ++g_present_calls;
    g_last_present = info;
    if (info->pResults) info->pResults[0] = g_present_result;
    return g_present_result;
}

VKAPI_ATTR VkResult VKAPI_CALL MockSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
    ++g_submits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL MockQueueIdle(VkQueue) { ++g_queue_idle; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL MockFenceStatus(VkDevice, VkFence) { return VK_NOT_READY; }

PFN_vkVoidFunction VKAPI_CALL MockGdpa(VkDevice, const char* name) {
#define MOCK(name_, function_) if (!std::strcmp(name, name_)) return reinterpret_cast<PFN_vkVoidFunction>(function_)
    MOCK("vkGetDeviceProcAddr", MockGdpa);
    MOCK("vkDestroyDevice", MockDestroyDevice);
    MOCK("vkGetDeviceQueue", MockGetQueue);
    MOCK("vkCreateSwapchainKHR", MockCreateSwapchain);
    MOCK("vkDestroySwapchainKHR", MockDestroySwapchain);
    MOCK("vkGetSwapchainImagesKHR", MockImages);
    MOCK("vkAcquireNextImageKHR", MockAcquire);
    MOCK("vkQueuePresentKHR", MockPresent);
    MOCK("vkQueueSubmit", MockSubmit);
    MOCK("vkQueueWaitIdle", MockQueueIdle);
    MOCK("vkGetFenceStatus", MockFenceStatus);
#undef MOCK
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL MockGipa(VkInstance, const char* name) {
#define MOCK(name_, function_) if (!std::strcmp(name, name_)) return reinterpret_cast<PFN_vkVoidFunction>(function_)
    MOCK("vkGetInstanceProcAddr", MockGipa);
    MOCK("vkGetDeviceProcAddr", MockGdpa);
    MOCK("vkCreateInstance", MockCreateInstance);
    MOCK("vkDestroyInstance", MockDestroyInstance);
    MOCK("vkEnumeratePhysicalDevices", MockEnumeratePhysical);
    MOCK("vkCreateDevice", MockCreateDevice);
    MOCK("vkQueuePresentKHR", MockPresent);
#undef MOCK
    return nullptr;
}

VkSwapchainCreateInfoKHR BaseSwapchain() {
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.minImageCount = 2;
    info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = {1280, 720};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    return info;
}

void TestPolicies() {
    Require(!IsPreviewProcess(), "test executable is NOT on preview allowlist");
    auto swap = BaseSwapchain();
    Require(SupportedSwapchain(swap), "ordinary SDR swapchain supported");
    swap.imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
    Require(SupportedSwapchain(swap), "sRGB image format supported");
    swap.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    Require(!SupportedSwapchain(swap), "HDR bypass before wait consumption");
    swap = BaseSwapchain();
    swap.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
    Require(!SupportedSwapchain(swap), "protected swapchain bypass");
    swap = BaseSwapchain();
    swap.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Require(!SupportedSwapchain(swap), "missing color-attachment usage bypass");
    swap = BaseSwapchain();
    swap.imageExtent = {0, 0};
    Require(!SupportedSwapchain(swap), "minimized swapchain bypass");
    swap = BaseSwapchain();
    swap.imageArrayLayers = 2;
    Require(!SupportedSwapchain(swap), "stereo bypass");
    swap = BaseSwapchain();
    VkSurfaceFullScreenExclusiveInfoEXT fullscreen{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    fullscreen.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    swap.pNext = &fullscreen;
    Require(SupportedSwapchain(swap), "fullscreen policy passed through untouched");
    VkImageFormatListCreateInfo formats{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    swap.pNext = &formats;
    Require(!SupportedSwapchain(swap), "unknown swapchain chain bypass");
    Require(SupportedPresentChain(nullptr), "empty present chain supported");
    VkPresentIdKHR id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    Require(SupportedPresentChain(&id), "present ID preserved");
    VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
    Require(!SupportedPresentChain(&regions), "incremental damage safely bypassed");
    VkSwapchainPresentFenceInfoEXT fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    Require(!SupportedPresentChain(&fence), "app-owned present fence not replaced");
    id.pNext = &regions;
    Require(!SupportedPresentChain(&id), "unsupported tail is not lost");
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    maintenance.swapchainMaintenance1 = VK_TRUE;
    device.pNext = &maintenance;
    Require(!MaintenanceEnabled(device), "feature without enabled extension isn't enough");
    const char* extension = "VK_KHR_swapchain_maintenance1";
    device.enabledExtensionCount = 1;
    device.ppEnabledExtensionNames = &extension;
    Require(MaintenanceEnabled(device), "KHR maintenance feature detected");
    maintenance.swapchainMaintenance1 = VK_FALSE;
    Require(!MaintenanceEnabled(device), "extension without enabled feature isn't enough");
    Require(PresentWasEnqueued(VK_ERROR_OUT_OF_DATE_KHR), "out-of-date still enqueues semaphore waits");
    Require(PresentWasEnqueued(VK_ERROR_SURFACE_LOST_KHR), "surface lost still enqueues waits");
    Require(!PresentWasEnqueued(VK_ERROR_OUT_OF_HOST_MEMORY), "OOM does not enqueue present waits");
}

void TestChainHeaderRegression() {
    // Без noinline/volatile и без выключения strict-aliasing: typed pNext stores
    // должны быть видны повторным walks и после aggressive inlining при -O3.
    VkPresentIdKHR id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    VkSwapchainPresentModeInfoEXT mode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
    VkSwapchainPresentFenceInfoEXT fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    id.pNext = &times;
    times.pNext = &mode;
    Require(SupportedPresentChain(&id), "mixed supported present chain");
    times.pNext = &regions;
    Require(!SupportedPresentChain(&id), "typed middle pNext store exposes unsupported tail at -O3");
    times.pNext = &mode;
    mode.pNext = &fence;
    Require(!SupportedPresentChain(&id), "deep app fence is not lost after typed tail store");
    mode.pNext = nullptr;
    Require(SupportedPresentChain(&id), "removing unsupported tail is visible to next walk");
    mode.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
    Require(!SupportedPresentChain(&id), "typed sType mutation is visible to header reader");
    mode.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT;
    Require(SupportedPresentChain(&id), "restored typed sType is visible to header reader");
    const ChainHeader copied = ReadChainHeader(&id);
    id.pNext = &regions;
    Require(copied.pNext == &times && ReadChainHeader(&id).pNext == &regions,
            "header snapshot is a value; current next pointer still refers to original live objects");
    id.pNext = &times;
    mode.pNext = &id;
    Require(!SupportedPresentChain(&id), "cyclic present chain fails bounded walk");
    mode.pNext = nullptr;
    Require(SupportedPresentChain(&id), "breaking cycle restores ordinary chain");

    VkLayerInstanceCreateInfo instance_link{};
    instance_link.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    instance_link.function = VK_LAYER_LINK_INFO;
    VkLayerInstanceCreateInfo instance_callback{};
    instance_callback.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    instance_callback.function = VK_LOADER_DATA_CALLBACK;
    instance_callback.pNext = &instance_link;
    VkDebugUtilsMessengerCreateInfoEXT instance_extension{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    instance_extension.pNext = &instance_callback;
    Require(InstanceChain(&instance_extension, VK_LAYER_LINK_INFO) == &instance_link,
            "instance walk skips actual extension and callback objects");
    Require(InstanceChain(&instance_extension, VK_LOADER_DATA_CALLBACK) == &instance_callback,
            "instance callback returns original object, not copied header address");
    instance_callback.pNext = nullptr;
    Require(InstanceChain(&instance_extension, VK_LAYER_LINK_INFO) == nullptr,
            "instance chain observes typed middle unlink");
    instance_callback.pNext = &instance_link;
    VkLayerInstanceLink instance_payload{};
    auto* found_instance = InstanceChain(&instance_extension, VK_LAYER_LINK_INFO);
    Require(found_instance == &instance_link, "instance chain relink is visible");
    found_instance->u.pLayerInfo = &instance_payload;
    Require(instance_link.u.pLayerInfo == &instance_payload,
            "loader mutation applies to actual typed instance object");

    VkLayerDeviceCreateInfo device_link{};
    device_link.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    device_link.function = VK_LAYER_LINK_INFO;
    VkLayerDeviceCreateInfo device_callback{};
    device_callback.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    device_callback.function = VK_LOADER_DATA_CALLBACK;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    maintenance.swapchainMaintenance1 = VK_TRUE;
    maintenance.pNext = &device_link; // feature uses mutable void*, loader nodes const void*.
    device_callback.pNext = &maintenance;
    VkDeviceGroupDeviceCreateInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};
    group.physicalDeviceCount = 1;
    group.pNext = &device_callback;
    Require(DeviceChain(&group, VK_LAYER_LINK_INFO) == &device_link,
            "device walk spans mixed const/mutable pNext object types");
    Require(DeviceChain(&group, VK_LOADER_DATA_CALLBACK) == &device_callback,
            "device callback points to live loader object");
    maintenance.pNext = nullptr;
    Require(DeviceChain(&group, VK_LAYER_LINK_INFO) == nullptr,
            "mutable feature pNext unlink is visible to device walk");
    maintenance.pNext = &device_link;
    VkLayerDeviceLink device_payload{};
    auto* found_device = DeviceChain(&group, VK_LAYER_LINK_INFO);
    Require(found_device == &device_link, "mutable feature pNext relink is visible");
    found_device->u.pLayerInfo = &device_payload;
    Require(device_link.u.pLayerInfo == &device_payload, "device link mutation is not applied to a temporary");

    const char* extension = "VK_EXT_swapchain_maintenance1";
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pNext = &group;
    device.enabledExtensionCount = 1;
    device.ppEnabledExtensionNames = &extension;
    Require(MaintenanceEnabled(device), "maintenance feature found behind typed loader node");
    maintenance.swapchainMaintenance1 = VK_FALSE;
    Require(!MaintenanceEnabled(device), "typed feature payload change is observed");
    maintenance.swapchainMaintenance1 = VK_TRUE;
    Require(MaintenanceEnabled(device), "typed feature payload restore is observed");
    device_callback.pNext = &device_link;
    Require(!MaintenanceEnabled(device), "maintenance walk observes typed middle unlink");
    device_callback.pNext = &maintenance;
    Require(MaintenanceEnabled(device), "maintenance walk observes typed middle relink");
    Require(SingleDeviceGroup(&group), "single-device group behind generic chain header");
    group.physicalDeviceCount = 2;
    Require(!SingleDeviceGroup(&group), "device-group payload change disables preview");
    group.physicalDeviceCount = 1;
    Require(SingleDeviceGroup(&group), "device-group payload restore permits preview");
    device_callback.pNext = nullptr;
    maintenance.pNext = &group;
    Require(SingleDeviceGroup(&maintenance), "device group found behind mutable feature pNext");
    group.physicalDeviceCount = 2;
    Require(!SingleDeviceGroup(&maintenance), "nested device-group restriction is not skipped");
    maintenance.pNext = nullptr;
    Require(SingleDeviceGroup(&maintenance), "device-group walk observes typed unlink");

    auto swap = BaseSwapchain();
    VkSurfaceFullScreenExclusiveInfoEXT fullscreen{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    VkSurfaceFullScreenExclusiveWin32InfoEXT win32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    VkImageFormatListCreateInfo formats{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    swap.pNext = &fullscreen;
    fullscreen.pNext = &win32;
    Require(SupportedSwapchain(swap), "mixed fullscreen swapchain headers preserved");
    win32.pNext = &formats;
    Require(!SupportedSwapchain(swap), "swapchain typed tail store exposes unknown extension");
    win32.pNext = nullptr;
    Require(SupportedSwapchain(swap), "swapchain walk observes typed tail removal");
    fullscreen.pNext = &formats;
    Require(!SupportedSwapchain(swap), "swapchain walk observes typed middle replacement");
    fullscreen.pNext = &win32;
    Require(SupportedSwapchain(swap), "swapchain walk observes typed middle restore");
}

void TestChainAndBypass() {
    VkNegotiateLayerInterface negotiate{};
    negotiate.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    negotiate.loaderLayerInterfaceVersion = 999;
    Require(vkNegotiateLoaderLayerInterfaceVersion(&negotiate) == VK_SUCCESS &&
            negotiate.loaderLayerInterfaceVersion == 2, "loader/layer version negotiation");
    negotiate.loaderLayerInterfaceVersion = 1;
    Require(vkNegotiateLoaderLayerInterfaceVersion(&negotiate) == VK_ERROR_INITIALIZATION_FAILED,
            "unsupported interface version rejected");
    uint32_t count = 0;
    Require(vkEnumerateInstanceLayerProperties(&count, nullptr) == VK_SUCCESS && count == 1,
            "layer enumeration count");
    VkLayerProperties layer{};
    Require(vkEnumerateInstanceLayerProperties(&count, &layer) == VK_SUCCESS &&
            !std::strcmp(layer.layerName, kLayerName), "enumerates exactly own layer");
    Require(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) == VK_ERROR_LAYER_NOT_PRESENT,
            "layer does not impersonate platform extensions");
    Require(vkEnumerateInstanceExtensionProperties(kLayerName, &count, nullptr) == VK_SUCCESS && count == 0,
            "no invented extensions");

    VkLayerInstanceLink link{};
    link.pfnNextGetInstanceProcAddr = MockGipa;
    VkLayerInstanceCreateInfo chain{};
    chain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    chain.function = VK_LAYER_LINK_INFO;
    chain.u.pLayerInfo = &link;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pNext = &chain;
    VkInstance instance = VK_NULL_HANDLE;
    Require(CreateInstance(&create, nullptr, &instance) == VK_SUCCESS, "instance creation through next chain");
    Require(g_instance_link_advanced, "instance link is advanced exactly once");
    Require(!FindInstance(instance)->preview, "non-demo instance remains passive");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    count = 1;
    Require(EnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && physical,
            "physical devices mapped to instance");

    VkLayerDeviceLink device_link{};
    device_link.pfnNextGetInstanceProcAddr = MockGipa;
    device_link.pfnNextGetDeviceProcAddr = MockGdpa;
    VkLayerDeviceCreateInfo callback{};
    callback.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    callback.function = VK_LOADER_DATA_CALLBACK;
    callback.u.pfnSetDeviceLoaderData = MockSetLoaderData;
    VkLayerDeviceCreateInfo device_chain{};
    device_chain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    device_chain.function = VK_LAYER_LINK_INFO;
    device_chain.u.pLayerInfo = &device_link;
    device_chain.pNext = &callback;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &device_chain;
    VkDevice device = VK_NULL_HANDLE;
    Require(CreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS, "device chain creation");
    Require(g_device_link_advanced, "device link is advanced exactly once");
    Require(vkGetDeviceProcAddr(device, "vkGetDeviceQueue2") == nullptr, "disabled command returns NULL");
    Require(vkGetDeviceProcAddr(device, "vkQueueSubmit") == reinterpret_cast<PFN_vkVoidFunction>(MockSubmit),
            "unintercepted commands forward unchanged");
    Require(vkGetDeviceProcAddr(device, "vkQueuePresentKHR") == reinterpret_cast<PFN_vkVoidFunction>(QueuePresent),
            "GDPA returns layer present interceptor, not a DLL-export hook");

    VkQueue queue = VK_NULL_HANDLE;
    GetDeviceQueue(device, 0, 0, &queue);
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    auto swap_info = BaseSwapchain();
    Require(CreateSwapchain(device, &swap_info, nullptr, &swapchain) == VK_SUCCESS, "passive swapchain create");
    VkSemaphore original_wait = NonDispatchable<VkSemaphore>(900);
    uint32_t index = 0;
    VkResult per_result = VK_NOT_READY;
    VkPresentIdKHR present_id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.pNext = &present_id;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &original_wait;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    present.pResults = &per_result;
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_last_present == &present,
            "non-demo forwards original present object");
    Require(per_result == VK_SUCCESS && g_submits == 0 && g_queue_idle == 0,
            "non-demo no GPU work or idle; pResults preserved");
    DestroySwapchain(device, swapchain, nullptr);

    // Внутренняя mock-проверка preview paths; реальный executable allowlist не меняется.
    auto state = FindDevice(device);
    state->preview = true;
    state->connected = true;
    state->valid_control = true;
    state->last_control_read = framescope::QpcNow();
    state->control.updated_qpc = state->last_control_read;
    state->control.flags = 0;
    GetDeviceQueue(device, 0, 0, &queue);
    Require(CreateSwapchain(device, &swap_info, nullptr, &swapchain) == VK_SUCCESS, "tracked SDR swapchain create");
    present.pSwapchains = &swapchain;
    auto swap = state->swapchains.at(swapchain);
    Require(AcquireNextImage(device, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) == VK_SUCCESS &&
            swap->images[index].acquired, "successful acquire records image without host wait");
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_last_present == &present,
            "disabled HUD preserves present/waits/pNext");
    Require(state->session.Snapshot().present_count == 1 && !swap->images[index].acquired,
            "only successful primary present is observed");
    g_present_result = VK_ERROR_OUT_OF_DATE_KHR;
    Require(QueuePresent(queue, &present) == VK_ERROR_OUT_OF_DATE_KHR &&
            state->session.Snapshot().present_count == 1, "failed present not counted as FPS");
    g_present_result = VK_SUBOPTIMAL_KHR;
    Require(QueuePresent(queue, &present) == VK_SUBOPTIMAL_KHR &&
            state->session.Snapshot().present_count == 2, "SUBOPTIMAL present is counted");
    g_present_result = VK_SUCCESS;

    state->control.flags = framescope::Enabled;
    state->control.updated_qpc = framescope::QpcNow();
    state->last_control_read = state->control.updated_qpc;
    state->queue_families.resize(1);
    state->queue_families[0].queueFlags = VK_QUEUE_TRANSFER_BIT;
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_submits == 0 && g_last_present == &present,
            "present-only queue bypass before wait consumption");
    VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
    present.pNext = &regions;
    state->queue_families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_submits == 0 && g_last_present == &present,
            "unsupported damage pNext preserves original call and waits");
    present.pNext = nullptr;
    AcquireNextImage(device, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index);
    swap->initialized = true;
    swap->queue = queue;
    swap->ring.resize(2);
    swap->ring[1].fence = NonDispatchable<VkFence>(777);
    swap->ring[1].in_flight = true;
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_submits == 0 && g_last_present == &present,
            "busy exact backend slot skips HUD without waiting or consuming original semaphore");
    swap->ring.clear();
    swap->initialized = false;
    state->control.flags = 0;
    present.waitSemaphoreCount = 0;
    present.pWaitSemaphores = nullptr;
    Require(QueuePresent(queue, &present) == VK_SUCCESS && g_last_present == &present,
            "zero original waits accepted on passthrough path");

    VkSwapchainKHR swaps[2]{swapchain, NonDispatchable<VkSwapchainKHR>(1000)};
    uint32_t indices[2]{};
    VkResult results[2]{};
    VkPresentInfoKHR batch = present;
    batch.swapchainCount = 2;
    batch.pSwapchains = swaps;
    batch.pImageIndices = indices;
    batch.pResults = results;
    Require(QueuePresent(queue, &batch) == VK_SUCCESS && g_last_present == &batch && g_submits == 0,
            "multiple-swapchain batch is passed through as one unchanged call");
    Require(g_queue_idle == 0, "all bypass cases avoid per-frame QueueWaitIdle");
    Require(g_present_calls == 9, "each accepted test call reaches downstream present exactly once");

    const uint64_t now = framescope::QpcNow();
    state->last_control_read = now;
    state->control.flags = framescope::Enabled;
    state->control.updated_qpc = now;
    Require(RefreshControl(*state, now), "fresh enabled mapping permits HUD");
    state->control.updated_qpc = now + framescope::QpcFrequency();
    Require(!RefreshControl(*state, now), "future control timestamp never enables HUD");
    if (now > 4 * framescope::QpcFrequency()) {
        state->control.updated_qpc = now - 4 * framescope::QpcFrequency();
        Require(!RefreshControl(*state, now), "stale mapping disables HUD without waiting for next read");
    }

    swap_info.oldSwapchain = swapchain;
    g_create_swap_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkSwapchainKHR failed = VK_NULL_HANDLE;
    Require(CreateSwapchain(device, &swap_info, nullptr, &failed) == VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "failed recreation status returned unchanged without retry");
    Require(swap->retired && state->primary == VK_NULL_HANDLE,
            "old swapchain retired even if replacement creation failed");
    g_create_swap_result = VK_SUCCESS;
    DestroySwapchain(device, swapchain, nullptr);
    DestroyDevice(device, nullptr);
    DestroyInstance(instance, nullptr);
    Require(g_creates == 2 && g_destroys == 2, "instance/device destroyed through matching next dispatch");
    Require(g_instances.empty() && g_devices.empty() && g_device_dispatch.empty() && g_physical.empty(),
            "dispatch maps cleaned after destroy");
}

void TestBuiltDll() {
    wchar_t path[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    Require(size && size < std::size(path), "test executable path available");
    wchar_t* basename = std::wcsrchr(path, L'\\');
    Require(basename != nullptr, "absolute test executable path");
    const std::wstring directory(path, static_cast<size_t>(basename - path) + 1);
    std::wstring dll_path = directory + L"FrameScope.Vulkan.dll";
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        Require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
                "adjacent DLL lookup failed only because file is absent, not because access was denied");
        // Root CMake кладёт EXE в artifacts/native/tests, DLL — в sibling bin.
        // Локальный .check layout остаётся adjacent. Никакого fallback после
        // ошибки LoadLibrary/security policy и никакого поиска через cwd/PATH.
        dll_path = directory + L"..\\bin\\FrameScope.Vulkan.dll";
    }
    HMODULE dll = LoadLibraryExW(dll_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) std::fprintf(stderr, "LoadLibraryExW failed (%lu): %ls\n", GetLastError(), dll_path.c_str());
    Require(dll != nullptr, "real built DLL loads without Vulkan SDK/import library");
    const char* exports[]{"vkNegotiateLoaderLayerInterfaceVersion", "vkGetInstanceProcAddr",
        "vkGetDeviceProcAddr", "vk_layerGetPhysicalDeviceProcAddr", "vkEnumerateInstanceLayerProperties",
        "vkEnumerateInstanceExtensionProperties", "vkEnumerateDeviceExtensionProperties"};
    for (const char* name : exports) Require(GetProcAddress(dll, name) != nullptr, name);
    auto negotiate_function = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
        GetProcAddress(dll, "vkNegotiateLoaderLayerInterfaceVersion"));
    VkNegotiateLayerInterface negotiate{};
    negotiate.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    negotiate.loaderLayerInterfaceVersion = 2;
    Require(negotiate_function(&negotiate) == VK_SUCCESS && negotiate.pfnGetDeviceProcAddr &&
            negotiate.pfnGetInstanceProcAddr && negotiate.pfnGetPhysicalDeviceProcAddr,
            "built DLL negotiates all dispatch entrypoints");
    Require(negotiate.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") != nullptr,
            "built DLL exposes creation entrypoint before instance exists");
    FreeLibrary(dll);
}

} // namespace

int main() {
    TestPolicies();
    TestChainHeaderRegression();
    TestChainAndBypass();
    TestBuiltDll();
    std::printf("FrameScope Vulkan: %u mock checks passed (no GPU/runtime certification).\n", g_checks);
    return 0;
}

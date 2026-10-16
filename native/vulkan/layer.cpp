// SPDX-License-Identifier: GPL-3.0-only
// FrameScope: собственный explicit Vulkan layer. Код сторонних HUD не используется.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#endif

#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "framescope/hud_core.hpp"
#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define FS_EXPORT extern "C" __declspec(dllexport) VKAPI_ATTR

FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance, const char*);
FS_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t*, VkLayerProperties*);
FS_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t*, VkExtensionProperties*);
FS_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);

namespace {

constexpr char kLayerName[] = "VK_LAYER_FRAMESCOPE_hud";
constexpr uint32_t kMaxImages = 16;
constexpr uint64_t kStaleSeconds = 3;

struct InstanceState;
struct DeviceState;
struct SwapchainState;

struct VulkanFailure { VkResult result; };

void Check(VkResult result) {
    if (result < 0) throw VulkanFailure{result};
}

void Debug(const char* text) noexcept {
    OutputDebugStringA("FrameScope.Vulkan: ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
}

enum Reason : uint64_t {
    UnsupportedSwapchain = 1ull << 0,
    UnsupportedBatch = 1ull << 1,
    UnsupportedPresentChain = 1ull << 2,
    PresentOnlyQueue = 1ull << 3,
    ChangedQueue = 1ull << 4,
    OtherDevice = 1ull << 5,
    NotAcquired = 1ull << 6,
    ResourcesBusy = 1ull << 7,
    InitializationFailed = 1ull << 8,
    SubmitFailed = 1ull << 9,
    PoisonedSemaphore = 1ull << 10,
};

struct DeviceDispatch {
#define FS_DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(QueuePresentKHR) \
    X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) \
    X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
    X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(GetFenceStatus) X(WaitForFences) X(ResetFences)
#define FS_DECLARE(name) PFN_vk##name name = nullptr;
    FS_DEVICE_FUNCTIONS(FS_DECLARE)
#undef FS_DECLARE
};

struct InstanceState {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_GetPhysicalDeviceProcAddr next_phys_gpa = nullptr;
    uint32_t api_version = VK_API_VERSION_1_0;
    bool preview = false;
};

struct QueueState {
    uint32_t family = 0;
    VkDeviceQueueCreateFlags flags = 0;
};

struct ImageState {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSemaphore finished = VK_NULL_HANDLE;
    // Новый acquire сам по себе не означает CPU completion. Сигнал finished повторяется
    // только ПОСЛЕ waits приложения в нашем submit, то есть после acquire dependency.
    bool acquired = false;
    bool poisoned = false;
};

struct RingSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;
};

struct SwapchainState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<uint32_t> families;
    uint32_t minimum_images = 2;
    bool supported = false;
    bool retired = false;
    bool disabled = false;
    bool initialized = false;
    bool backend_initialized = false;
    bool legacy_present_used = false;
    uint64_t reasons = 0;
    uint64_t last_frame_qpc = 0;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    ImGuiContext* context = nullptr;
    // Backend 1.91.9b сначала выбирает slot 1. Это именно его ring, НЕ image index.
    uint32_t ring_cursor = 0;
    std::vector<ImageState> images;
    std::vector<RingSlot> ring;
    std::vector<VkFence> present_fences;
};

struct DeviceState {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::shared_ptr<InstanceState> instance;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    DeviceDispatch vk;
    bool preview = false;
    bool maintenance = false;
    bool lost = false;
    std::vector<VkQueueFamilyProperties> queue_families;
    std::unordered_map<VkQueue, QueueState> queues;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainState>> swapchains;
    VkSwapchainKHR primary = VK_NULL_HANDLE;
    framescope::Session session;
    bool connected = false;
    bool valid_control = false;
    uint64_t last_control_read = 0;
    framescope::ControlData control{};
    uint64_t reasons = 0;
};

// maps_mutex не удерживается во время вызовов драйвера. render_mutex защищает только
// наши resources и глобальные function pointers pinned backend 1.91.9b.
std::mutex g_maps_mutex;
std::mutex g_render_mutex;
std::unordered_map<VkInstance, std::shared_ptr<InstanceState>> g_instances;
std::unordered_map<void*, std::shared_ptr<InstanceState>> g_instance_dispatch;
std::unordered_map<VkPhysicalDevice, std::weak_ptr<InstanceState>> g_physical;
std::unordered_map<VkDevice, std::shared_ptr<DeviceState>> g_devices;
std::unordered_map<void*, std::shared_ptr<DeviceState>> g_device_dispatch;
DeviceState* g_backend_owner = nullptr;

template <typename Handle>
void* DispatchKey(Handle handle) noexcept {
    void* key = nullptr;
    // Loader ABI задаёт dispatch pointer в первом word opaque object, но реальный
    // C++ тип object неизвестен. Читаем representation, не разыменовываем void**.
    if (handle) std::memcpy(&key, handle, sizeof(key));
    return key;
}

std::shared_ptr<InstanceState> FindInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    const auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : it->second;
}

std::shared_ptr<InstanceState> FindPhysical(VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    const auto it = g_physical.find(physical);
    if (it != g_physical.end()) {
        if (auto result = it->second.lock()) return result;
    }
    const auto dispatch = g_instance_dispatch.find(DispatchKey(physical));
    return dispatch == g_instance_dispatch.end() ? nullptr : dispatch->second;
}

template <typename Handle>
std::shared_ptr<DeviceState> FindDevice(Handle object) {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    const auto it = g_device_dispatch.find(DispatchKey(object));
    return it == g_device_dispatch.end() ? nullptr : it->second;
}

void Report(uint64_t& reasons, Reason reason, const char* text) noexcept {
    if ((reasons & reason) == 0) {
        reasons |= reason;
        Debug(text);
    }
}

bool IsPreviewProcess() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"FrameScope.Demo.exe") == 0 &&
           framescope::ValidSession(framescope::SessionFromEnvironment());
}

template <typename T>
T InstanceFunction(const InstanceState& state, const char* name) {
    return reinterpret_cast<T>(state.next_gipa(state.instance, name));
}

struct ChainHeader {
    VkStructureType sType;
    const void* pNext;
};

static_assert(std::is_trivially_copyable_v<ChainHeader>);
static_assert(sizeof(ChainHeader) == sizeof(VkBaseInStructure));
static_assert(offsetof(ChainHeader, sType) == offsetof(VkBaseInStructure, sType));
static_assert(offsetof(ChainHeader, pNext) == offsetof(VkBaseInStructure, pNext));
static_assert(offsetof(ChainHeader, pNext) == offsetof(VkBaseOutStructure, pNext));

template <typename T>
constexpr bool HasChainHeaderLayout() noexcept {
    return std::is_standard_layout_v<T> && sizeof(T) >= sizeof(ChainHeader) &&
           offsetof(T, sType) == offsetof(ChainHeader, sType) &&
           offsetof(T, pNext) == offsetof(ChainHeader, pNext) &&
           sizeof(T::pNext) == sizeof(ChainHeader::pNext);
}

static_assert(HasChainHeaderLayout<VkLayerInstanceCreateInfo>());
static_assert(HasChainHeaderLayout<VkLayerDeviceCreateInfo>());
static_assert(HasChainHeaderLayout<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>());
static_assert(HasChainHeaderLayout<VkDeviceGroupDeviceCreateInfo>());
static_assert(HasChainHeaderLayout<VkSurfaceFullScreenExclusiveInfoEXT>());
static_assert(HasChainHeaderLayout<VkSurfaceFullScreenExclusiveWin32InfoEXT>());
static_assert(HasChainHeaderLayout<VkPresentIdKHR>());
static_assert(HasChainHeaderLayout<VkPresentTimesInfoGOOGLE>());
static_assert(HasChainHeaderLayout<VkSwapchainPresentModeInfoEXT>());
#ifdef VK_KHR_present_id2
static_assert(HasChainHeaderLayout<VkPresentId2KHR>());
#endif

ChainHeader ReadChainHeader(const void* node) noexcept {
    ChainHeader header{};
    // Общий prefix Vulkan — ABI layout, а НЕ C++ inheritance/common object type.
    // VkPresentIdKHR/VkLayer* и VkBaseInStructure не alias-compatible. memcpy
    // сохраняет typed pNext stores при -O3/strict-aliasing; header возвращается
    // значением, а его pNext по-прежнему указывает в исходную live chain.
    if (node) std::memcpy(&header, node, sizeof(header));
    return header;
}

VkLayerInstanceCreateInfo* InstanceChain(const void* next, VkLayerFunction function) {
    const void* node = next;
    while (node) {
        const ChainHeader header = ReadChainHeader(node);
        if (header.sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto info = const_cast<VkLayerInstanceCreateInfo*>(
                static_cast<const VkLayerInstanceCreateInfo*>(node));
            if (info->function == function) return info;
        }
        node = header.pNext;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* DeviceChain(const void* next, VkLayerFunction function) {
    const void* node = next;
    while (node) {
        const ChainHeader header = ReadChainHeader(node);
        if (header.sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto info = const_cast<VkLayerDeviceCreateInfo*>(
                static_cast<const VkLayerDeviceCreateInfo*>(node));
            if (info->function == function) return info;
        }
        node = header.pNext;
    }
    return nullptr;
}

bool MaintenanceEnabled(const VkDeviceCreateInfo& info) {
    bool extension = false;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        const char* name = info.ppEnabledExtensionNames[i];
        if (!std::strcmp(name, "VK_EXT_swapchain_maintenance1") ||
            !std::strcmp(name, "VK_KHR_swapchain_maintenance1")) extension = true;
    }
    const void* node = info.pNext;
    while (node) {
        const ChainHeader header = ReadChainHeader(node);
        if (header.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
            const auto* feature = static_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(node);
            return extension && feature->swapchainMaintenance1 == VK_TRUE;
        }
        node = header.pNext;
    }
    return false;
}

bool SingleDeviceGroup(const void* next) noexcept {
    const void* node = next;
    while (node) {
        const ChainHeader header = ReadChainHeader(node);
        if (header.sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
            static_cast<const VkDeviceGroupDeviceCreateInfo*>(node)->physicalDeviceCount != 1)
            return false;
        node = header.pNext;
    }
    return true;
}

bool IsSdrFormat(VkFormat format) noexcept {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

bool SupportedSwapchain(const VkSwapchainCreateInfoKHR& info) noexcept {
    if (info.flags != 0 || info.imageArrayLayers != 1 ||
        info.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        !IsSdrFormat(info.imageFormat) || !info.imageExtent.width || !info.imageExtent.height ||
        !(info.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        info.compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        info.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info.presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
        info.presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) return false;
    const void* node = info.pNext;
    while (node) {
        const ChainHeader header = ReadChainHeader(node);
        // Политика exclusive fullscreen принадлежит приложению и не изменяется.
        if (header.sType != VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT &&
            header.sType != VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT) return false;
        node = header.pNext;
    }
    return true;
}

bool SupportedPresentChain(const void* next) noexcept {
    const void* node = next;
    for (uint32_t count = 0; node && count < 64; ++count) {
        const ChainHeader header = ReadChainHeader(node);
        switch (header.sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT:
            break;
#ifdef VK_KHR_present_id2
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            break;
#endif
        default:
            // Включая incremental damage, device groups и app-owned present fence:
            // не теряем pNext и не рисуем вне объявленного damage region.
            return false;
        }
        node = header.pNext;
    }
    return node == nullptr;
}

bool PresentWasEnqueued(VkResult result) noexcept {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
           result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR ||
           result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT
#ifdef VK_EXT_present_timing
           || result == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT
#endif
           ;
}

bool RefreshControl(DeviceState& state, uint64_t now) {
    const uint64_t frequency = framescope::QpcFrequency();
    if (!state.last_control_read || now - state.last_control_read >= frequency / 2) {
        state.last_control_read = now;
        if (!state.connected) state.connected = state.session.ConnectFromEnvironment();
        state.valid_control = state.connected && state.session.ReadControl(state.control);
    }
    return state.valid_control && state.control.updated_qpc != 0 &&
           now >= state.control.updated_qpc &&
           now - state.control.updated_qpc <= kStaleSeconds * frequency &&
           (state.control.flags & framescope::Enabled) != 0;
}

struct ContextScope {
    ImGuiContext* previous;
    explicit ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

VKAPI_ATTR VkResult VKAPI_CALL BackendAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    const auto state = FindDevice(device);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->vk.AllocateCommandBuffers(device, info, buffers);
    if (result != VK_SUCCESS) return result;
    // В том числе command buffer загрузки font atlas внутри ImGui backend.
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        const VkResult initialized = state->set_loader_data(device, buffers[i]);
        if (initialized != VK_SUCCESS) {
            state->vk.FreeCommandBuffers(device, info->commandPool, info->commandBufferCount, buffers);
            return initialized;
        }
    }
    return VK_SUCCESS;
}

PFN_vkVoidFunction BackendLoader(const char* name, void* user) {
    auto& state = *static_cast<DeviceState*>(user);
    if (!std::strcmp(name, "vkAllocateCommandBuffers"))
        return reinterpret_cast<PFN_vkVoidFunction>(BackendAllocateCommandBuffers);
    // Никакого vkGet*ProcAddr через системный loader: только нижняя часть chain.
    const auto device_function = state.next_gdpa(state.device, name);
    return device_function ? device_function : state.instance->next_gipa(state.instance->instance, name);
}

void ReapPresentFences(DeviceState& state, SwapchainState& swap) {
    auto it = swap.present_fences.begin();
    while (it != swap.present_fences.end()) {
        const VkResult status = state.vk.GetFenceStatus(state.device, *it);
        if (status == VK_SUCCESS) {
            state.vk.DestroyFence(state.device, *it, nullptr);
            it = swap.present_fences.erase(it);
        } else {
            if (status == VK_ERROR_DEVICE_LOST) state.lost = true;
            ++it;
        }
    }
}

void DestroyResources(DeviceState& state, SwapchainState& swap) noexcept {
    // Вызывается только при destroy/recreation cleanup, никогда на каждом кадре.
    try {
        if (!state.lost) {
            for (auto& slot : swap.ring) {
                if (slot.in_flight) {
                    const VkResult result = state.vk.WaitForFences(
                        state.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                    if (result == VK_ERROR_DEVICE_LOST) state.lost = true;
                }
            }
            for (VkFence fence : swap.present_fences) {
                if (state.lost) break;
                const VkResult result = state.vk.WaitForFences(state.device, 1, &fence, VK_TRUE, UINT64_MAX);
                if (result == VK_ERROR_DEVICE_LOST) state.lost = true;
            }
            if ((swap.legacy_present_used || (swap.backend_initialized && !swap.initialized)) &&
                swap.queue && !state.lost) {
                // Legacy demo-path: de-facto WSI teardown convention, НЕ универсальное
                // spec-доказательство освобождения presentation resources (см. README).
                const VkResult result = state.vk.QueueWaitIdle(swap.queue);
                if (result == VK_ERROR_DEVICE_LOST) state.lost = true;
            }
        }
        if (swap.context) {
            ContextScope context(swap.context);
            if (swap.backend_initialized) ImGui_ImplVulkan_Shutdown();
        }
    } catch (...) {
        Debug("exception during renderer cleanup; destroying remaining owned handles");
    }
    if (swap.context) {
        ImGui::DestroyContext(swap.context);
        swap.context = nullptr;
    }
    for (VkFence fence : swap.present_fences) state.vk.DestroyFence(state.device, fence, nullptr);
    swap.present_fences.clear();
    for (auto& slot : swap.ring) {
        if (slot.fence) state.vk.DestroyFence(state.device, slot.fence, nullptr);
        if (slot.pool) state.vk.DestroyCommandPool(state.device, slot.pool, nullptr);
    }
    swap.ring.clear();
    for (auto& image : swap.images) {
        if (image.finished) state.vk.DestroySemaphore(state.device, image.finished, nullptr);
        if (image.framebuffer) state.vk.DestroyFramebuffer(state.device, image.framebuffer, nullptr);
        if (image.view) state.vk.DestroyImageView(state.device, image.view, nullptr);
        image.finished = VK_NULL_HANDLE;
        image.framebuffer = VK_NULL_HANDLE;
        image.view = VK_NULL_HANDLE;
    }
    if (swap.render_pass) state.vk.DestroyRenderPass(state.device, swap.render_pass, nullptr);
    swap.render_pass = VK_NULL_HANDLE;
    swap.initialized = false;
    swap.backend_initialized = false;
}

bool InitializeResources(DeviceState& state, SwapchainState& swap, VkQueue queue, uint32_t family) {
    if (!state.set_loader_data) {
        Debug("bypass: loader device-data callback unavailable");
        return false;
    }
    if (g_backend_owner && g_backend_owner != &state) {
        Report(state.reasons, OtherDevice, "bypass: pinned ImGui backend supports one preview device");
        return false;
    }
    g_backend_owner = &state;
    swap.queue = queue;
    swap.queue_family = family;
    const auto get_format = InstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>(
        *state.instance, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties properties{};
    get_format(state.physical, swap.format, &properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    if ((properties.optimalTilingFeatures & required) != required) return false;

    VkAttachmentDescription attachment{};
    attachment.format = swap.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    Check(state.vk.CreateRenderPass(state.device, &pass, nullptr, &swap.render_pass));

    swap.ring.resize(swap.images.size());
    for (auto& image : swap.images) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swap.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        Check(state.vk.CreateImageView(state.device, &view, nullptr, &image.view));
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = swap.render_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &image.view;
        framebuffer.width = swap.extent.width;
        framebuffer.height = swap.extent.height;
        framebuffer.layers = 1;
        Check(state.vk.CreateFramebuffer(state.device, &framebuffer, nullptr, &image.framebuffer));
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(state.vk.CreateSemaphore(state.device, &semaphore, nullptr, &image.finished));
    }
    for (auto& slot : swap.ring) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family;
        Check(state.vk.CreateCommandPool(state.device, &pool, nullptr, &slot.pool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = slot.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(BackendAllocateCommandBuffers(state.device, &allocate, &slot.command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(state.vk.CreateFence(state.device, &fence, nullptr, &slot.fence));
    }

    const ImGuiContext* previous = ImGui::GetCurrentContext();
    swap.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(const_cast<ImGuiContext*>(previous));
    ContextScope context(swap.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "framescope-vulkan-layer";
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(static_cast<float>(swap.extent.width), static_cast<float>(swap.extent.height));
    ImGui::StyleColorsDark();
    // Фиксированный atlas установленных системных шрифтов: не обновляется на каждом кадре.
    framescope::ConfigureHudFonts();
    if (!ImGui_ImplVulkan_LoadFunctions(state.instance->api_version, BackendLoader, &state))
        throw std::runtime_error("ImGui Vulkan function loading failed");
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = state.instance->api_version;
    init.Instance = state.instance->instance;
    init.PhysicalDevice = state.physical;
    init.Device = state.device;
    init.QueueFamily = family;
    init.Queue = queue;
    init.DescriptorPoolSize = 16;
    init.RenderPass = swap.render_pass;
    init.MinImageCount = std::max(2u, std::min(swap.minimum_images, static_cast<uint32_t>(swap.images.size())));
    init.ImageCount = static_cast<uint32_t>(swap.images.size());
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = false;
    init.CheckVkResultFn = Check;
    init.MinAllocationSize = 1024 * 1024;
    // Init может бросить через Check после создания backend state: cleanup всё равно нужен.
    try {
        if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan init failed");
        swap.backend_initialized = true;
        if (!ImGui_ImplVulkan_CreateFontsTexture()) throw std::runtime_error("font upload failed");
    } catch (...) {
        swap.backend_initialized = io.BackendRendererUserData != nullptr;
        throw;
    }
    swap.initialized = true;
    Debug(state.maintenance ? "HUD initialized: present-fence lifecycle" :
                            "HUD initialized: controlled-demo legacy teardown (not universal WSI certification)");
    return true;
}

void LinearizeSrgbVertices(ImDrawData& data, VkFormat format) {
    if (format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_B8G8R8A8_SRGB) return;
    static const auto lookup = [] {
        std::array<unsigned int, 256> result{};
        for (unsigned int i = 0; i < result.size(); ++i) {
            const double encoded = i / 255.0;
            const double linear = encoded <= 0.04045 ? encoded / 12.92 :
                std::pow((encoded + 0.055) / 1.055, 2.4);
            result[i] = static_cast<unsigned int>(std::lround(linear * 255.0));
        }
        return result;
    }();
    for (int list = 0; list < data.CmdListsCount; ++list) {
        for (ImDrawVert& vertex : data.CmdLists[list]->VtxBuffer) {
            const ImU32 old = vertex.col;
            vertex.col = (lookup[(old >> IM_COL32_R_SHIFT) & 255] << IM_COL32_R_SHIFT) |
                         (lookup[(old >> IM_COL32_G_SHIFT) & 255] << IM_COL32_G_SHIFT) |
                         (lookup[(old >> IM_COL32_B_SHIFT) & 255] << IM_COL32_B_SHIFT) |
                         (old & IM_COL32_A_MASK);
        }
    }
}

struct PreparedPresent {
    std::shared_ptr<SwapchainState> swap;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkFence present_fence = VK_NULL_HANDLE;
    bool submitted = false;
    VkResult submit_result = VK_SUCCESS;
    double cpu_us = 0.0;
};

PreparedPresent PreparePresent(DeviceState& state, VkQueue queue, const VkPresentInfoKHR& present,
                               uint64_t timestamp) {
    PreparedPresent prepared;
    // Все проверки и allocation выполняются ДО consumption исходных binary waits.
    if (present.swapchainCount != 1) {
        Report(state.reasons, UnsupportedBatch, "bypass: multiple swapchains in one present batch");
        return prepared;
    }
    const auto found = state.swapchains.find(present.pSwapchains[0]);
    if (found == state.swapchains.end()) return prepared;
    auto& swap = *found->second;
    if (!swap.supported) {
        Report(swap.reasons, UnsupportedSwapchain,
               "bypass: requires opaque identity SDR 8-bit COLOR_ATTACHMENT swapchain; no protected/HDR/shared/stereo");
        return prepared;
    }
    if (state.primary == VK_NULL_HANDLE && !swap.retired) state.primary = swap.swapchain;
    if (state.primary != swap.swapchain) return prepared;
    prepared.swap = found->second;
    const bool enabled = RefreshControl(state, timestamp);
    if (!enabled || swap.disabled || state.lost) return prepared;
    if (!SupportedPresentChain(present.pNext)) {
        Report(swap.reasons, UnsupportedPresentChain, "bypass: unsupported present pNext (original chain preserved)");
        return prepared;
    }
    const auto queue_state = state.queues.find(queue);
    if (queue_state == state.queues.end() || queue_state->second.flags != 0 ||
        queue_state->second.family >= state.queue_families.size() ||
        !(state.queue_families[queue_state->second.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        Report(swap.reasons, PresentOnlyQueue, "bypass: unknown/protected/present-only queue cannot render graphics");
        return prepared;
    }
    const uint32_t family = queue_state->second.family;
    if (swap.initialized && swap.queue != queue) {
        Report(swap.reasons, ChangedQueue, "bypass: selected swapchain changed graphics/present queue");
        return prepared;
    }
    if (swap.sharing == VK_SHARING_MODE_CONCURRENT &&
        std::find(swap.families.begin(), swap.families.end(), family) == swap.families.end()) return prepared;
    const uint32_t image_index = present.pImageIndices[0];
    if (image_index >= swap.images.size() || !swap.images[image_index].acquired) {
        Report(swap.reasons, NotAcquired, "bypass: no intercepted successful acquire for presented image");
        return prepared;
    }
    auto& image = swap.images[image_index];
    if (image.poisoned) {
        Report(swap.reasons, PoisonedSemaphore, "bypass: failed present left private semaphore unavailable");
        return prepared;
    }
    try {
        if (!swap.initialized && !InitializeResources(state, swap, queue, family)) {
            swap.disabled = true;
            DestroyResources(state, swap);
            Report(swap.reasons, InitializationFailed, "bypass: renderer initialization unavailable");
            return prepared;
        }
        ReapPresentFences(state, swap);
        if (state.lost) return prepared;
        // Не накапливаем бесконечный список при задержанном presentation engine.
        if (swap.present_fences.size() >= 64) return prepared;
        const uint32_t slot_index = (swap.ring_cursor + 1) % static_cast<uint32_t>(swap.ring.size());
        RingSlot& slot = swap.ring[slot_index];
        if (slot.in_flight) {
            const VkResult status = state.vk.GetFenceStatus(state.device, slot.fence);
            if (status == VK_NOT_READY) {
                Report(swap.reasons, ResourcesBusy, "bypass this HUD frame: exact ImGui ring slot still in flight");
                return prepared;
            }
            Check(status);
            slot.in_flight = false;
        }
        ContextScope context(swap.context);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(swap.extent.width), static_cast<float>(swap.extent.height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        const double delta = swap.last_frame_qpc ?
            static_cast<double>(timestamp - swap.last_frame_qpc) / framescope::QpcFrequency() : 1.0 / 60.0;
        io.DeltaTime = static_cast<float>(std::clamp(delta, 0.000001, 0.1));
        swap.last_frame_qpc = timestamp;
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        framescope::DrawHud(state.control, state.session.Snapshot(), io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (!draw_data || draw_data->TotalVtxCount == 0) return prepared;
        LinearizeSrgbVertices(*draw_data, swap.format);
        Check(state.vk.ResetCommandPool(state.device, slot.pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(state.vk.BeginCommandBuffer(slot.command, &begin));
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        state.vk.CmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = swap.render_pass;
        render.framebuffer = image.framebuffer;
        render.renderArea.extent = swap.extent;
        state.vk.CmdBeginRenderPass(slot.command, &render, VK_SUBPASS_CONTENTS_INLINE);
        // Advance синхронно с внутренним ring ImGui, даже если последующий submit ошибётся.
        swap.ring_cursor = slot_index;
        ImGui_ImplVulkan_RenderDrawData(draw_data, slot.command);
        state.vk.CmdEndRenderPass(slot.command);
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        state.vk.CmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        Check(state.vk.EndCommandBuffer(slot.command));
        std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (state.maintenance) {
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            Check(state.vk.CreateFence(state.device, &fence, nullptr, &prepared.present_fence));
            // reserve до QueueSubmit: после consumption waits bad_alloc недопустим.
            swap.present_fences.reserve(swap.present_fences.size() + 1);
        }
        Check(state.vk.ResetFences(state.device, 1, &slot.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &image.finished;
        // Точка невозврата: нельзя fallback с исходными waits после этого submit.
        prepared.submit_result = state.vk.QueueSubmit(queue, 1, &submit, slot.fence);
        if (prepared.submit_result != VK_SUCCESS) {
            swap.disabled = true;
            state.lost = prepared.submit_result == VK_ERROR_DEVICE_LOST;
            if (prepared.present_fence) state.vk.DestroyFence(state.device, prepared.present_fence, nullptr);
            prepared.present_fence = VK_NULL_HANDLE;
            Report(swap.reasons, SubmitFailed, "HUD QueueSubmit failed; original present is NOT retried");
            return prepared;
        }
        slot.in_flight = true;
        prepared.submitted = true;
        prepared.semaphore = image.finished;
        prepared.cpu_us = static_cast<double>(framescope::QpcNow() - timestamp) * 1e6 / framescope::QpcFrequency();
        return prepared;
    } catch (const VulkanFailure& error) {
        if (error.result == VK_ERROR_DEVICE_LOST) state.lost = true;
    } catch (...) {
        Debug("renderer exception before original wait consumption");
    }
    if (prepared.present_fence) state.vk.DestroyFence(state.device, prepared.present_fence, nullptr);
    prepared.present_fence = VK_NULL_HANDLE;
    swap.disabled = true;
    // Собственный неисполненный command buffer может остаться recording; pool можно
    // уничтожить после ожидания уже submitted slots, но исходный present не трогаем.
    Report(swap.reasons, InitializationFailed, "HUD disabled after resource/recording failure; original present preserved");
    return prepared;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* chain = InstanceChain(info->pNext, VK_LAYER_LINK_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_phys = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    std::shared_ptr<InstanceState> state;
    try {
        state = std::make_shared<InstanceState>();
        state->next_gipa = next_gipa;
        state->next_phys_gpa = next_phys;
        state->api_version = info->pApplicationInfo && info->pApplicationInfo->apiVersion ?
            info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        state->preview = IsPreviewProcess();
    } catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    state->instance = *instance;
    try {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_instances.emplace(*instance, state);
        g_instance_dispatch.emplace(DispatchKey(*instance), state);
    } catch (...) {
        void* key = DispatchKey(*instance);
        const auto destroy = InstanceFunction<PFN_vkDestroyInstance>(*state, "vkDestroyInstance");
        destroy(*instance, allocator);
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_instances.erase(*instance);
        g_instance_dispatch.erase(key);
        *instance = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto state = FindInstance(instance);
    if (!state) return;
    void* key = DispatchKey(instance);
    InstanceFunction<PFN_vkDestroyInstance>(*state, "vkDestroyInstance")(instance, allocator);
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    g_instances.erase(instance);
    g_instance_dispatch.erase(key);
    for (auto it = g_physical.begin(); it != g_physical.end();) {
        if (it->second.lock() == state) it = g_physical.erase(it); else ++it;
    }
}

void RememberPhysical(const std::shared_ptr<InstanceState>& state, VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    g_physical[physical] = state;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t* count,
                                                       VkPhysicalDevice* devices) {
    const auto state = FindInstance(instance);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = InstanceFunction<PFN_vkEnumeratePhysicalDevices>(*state,
        "vkEnumeratePhysicalDevices")(instance, count, devices);
    if (devices && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        try { for (uint32_t i = 0; i < *count; ++i) RememberPhysical(state, devices[i]); } catch (...) {}
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* count,
                                                            VkPhysicalDeviceGroupProperties* groups) {
    const auto state = FindInstance(instance);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    auto function = InstanceFunction<PFN_vkEnumeratePhysicalDeviceGroups>(*state, "vkEnumeratePhysicalDeviceGroups");
    if (!function) function = InstanceFunction<PFN_vkEnumeratePhysicalDeviceGroups>(*state, "vkEnumeratePhysicalDeviceGroupsKHR");
    if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(instance, count, groups);
    if (groups && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        try {
            for (uint32_t i = 0; i < *count; ++i)
                for (uint32_t j = 0; j < groups[i].physicalDeviceCount; ++j)
                    RememberPhysical(state, groups[i].physicalDevices[j]);
        } catch (...) {}
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    const auto instance = FindPhysical(physical);
    auto* chain = DeviceChain(info->pNext, VK_LAYER_LINK_INFO);
    auto* callback = DeviceChain(info->pNext, VK_LOADER_DATA_CALLBACK);
    if (!instance || !chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(instance->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    std::shared_ptr<DeviceState> state;
    try {
        state = std::make_shared<DeviceState>();
        state->instance = instance;
        state->physical = physical;
        state->next_gdpa = next_gdpa;
        state->set_loader_data = callback ? callback->u.pfnSetDeviceLoaderData : nullptr;
        state->preview = instance->preview;
        state->maintenance = MaintenanceEnabled(*info);
        if (!SingleDeviceGroup(info->pNext)) state->preview = false;
        if (state->preview) {
            const auto families = InstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                *instance, "vkGetPhysicalDeviceQueueFamilyProperties");
            uint32_t count = 0;
            families(physical, &count, nullptr);
            state->queue_families.resize(count);
            families(physical, &count, state->queue_families.data());
            state->queue_families.resize(count);
        }
    } catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS) return result;
    state->device = *device;
#define FS_LOAD(name) state->vk.name = reinterpret_cast<PFN_vk##name>(next_gdpa(*device, "vk" #name));
    FS_DEVICE_FUNCTIONS(FS_LOAD)
#undef FS_LOAD
    try {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_devices.emplace(*device, state);
        g_device_dispatch.emplace(DispatchKey(*device), state);
    } catch (...) {
        void* key = DispatchKey(*device);
        state->vk.DestroyDevice(*device, allocator);
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_devices.erase(*device);
        g_device_dispatch.erase(key);
        *device = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    const auto state = FindDevice(device);
    if (!state) return;
    void* key = DispatchKey(device);
    if (state->preview) {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        for (auto& entry : state->swapchains) DestroyResources(*state, *entry.second);
        state->swapchains.clear();
        if (g_backend_owner == state.get()) g_backend_owner = nullptr;
    }
    state->vk.DestroyDevice(device, allocator);
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    g_devices.erase(device);
    g_device_dispatch.erase(key);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    const auto state = FindDevice(device);
    if (!state) return;
    state->vk.GetDeviceQueue(device, family, index, queue);
    if (state->preview && *queue) {
        try {
            std::lock_guard<std::mutex> lock(g_render_mutex);
            state->queues[*queue] = QueueState{family, 0};
        } catch (...) { Debug("queue tracking allocation failed; HUD bypasses untracked queue"); }
    }
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    const auto state = FindDevice(device);
    if (!state || !state->vk.GetDeviceQueue2) return;
    state->vk.GetDeviceQueue2(device, info, queue);
    if (state->preview && *queue) {
        try {
            std::lock_guard<std::mutex> lock(g_render_mutex);
            state->queues[*queue] = QueueState{info->queueFamilyIndex, info->flags};
        } catch (...) { Debug("queue tracking allocation failed; HUD bypasses untracked queue"); }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    const auto state = FindDevice(device);
    if (!state || !state->vk.CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
    // Ни imageUsage, ни pNext, ни oldSwapchain не переписываются и create не повторяется.
    const VkResult result = state->vk.CreateSwapchainKHR(device, info, allocator, swapchain);
    if (!state->preview) return result;
    try {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        if (info->oldSwapchain) {
            const auto old = state->swapchains.find(info->oldSwapchain);
            if (old != state->swapchains.end()) old->second->retired = true;
            if (state->primary == info->oldSwapchain) state->primary = VK_NULL_HANDLE;
        }
        if (result != VK_SUCCESS) return result;
        auto swap = std::make_shared<SwapchainState>();
        swap->swapchain = *swapchain;
        swap->format = info->imageFormat;
        swap->extent = info->imageExtent;
        swap->sharing = info->imageSharingMode;
        swap->minimum_images = info->minImageCount;
        swap->supported = SupportedSwapchain(*info);
        if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT)
            swap->families.assign(info->pQueueFamilyIndices, info->pQueueFamilyIndices + info->queueFamilyIndexCount);
        if (swap->supported) {
            uint32_t count = 0;
            const VkResult counted = state->vk.GetSwapchainImagesKHR(device, *swapchain, &count, nullptr);
            if (counted != VK_SUCCESS || count < 2 || count > kMaxImages) swap->supported = false;
            else {
                std::vector<VkImage> images(count);
                const VkResult fetched = state->vk.GetSwapchainImagesKHR(device, *swapchain, &count, images.data());
                if (fetched != VK_SUCCESS) swap->supported = false;
                else {
                    swap->images.resize(count);
                    for (uint32_t i = 0; i < count; ++i) swap->images[i].image = images[i];
                }
            }
        }
        state->swapchains.emplace(*swapchain, std::move(swap));
    } catch (...) { Debug("swapchain tracking unavailable; original successful creation preserved"); }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                          const VkAllocationCallbacks* allocator) {
    const auto state = FindDevice(device);
    if (!state || !state->vk.DestroySwapchainKHR) return;
    if (state->preview) {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        const auto found = state->swapchains.find(swapchain);
        if (found != state->swapchains.end()) {
            DestroyResources(*state, *found->second);
            state->swapchains.erase(found);
        }
        if (state->primary == swapchain) state->primary = VK_NULL_HANDLE;
    }
    state->vk.DestroySwapchainKHR(device, swapchain, allocator);
}

void Acquired(DeviceState& state, VkSwapchainKHR swapchain, uint32_t index) {
    std::lock_guard<std::mutex> lock(g_render_mutex);
    const auto found = state.swapchains.find(swapchain);
    if (found != state.swapchains.end() && index < found->second->images.size())
        found->second->images[index].acquired = true;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    const auto state = FindDevice(device);
    if (!state || !state->vk.AcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->vk.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
    if (state->preview && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) Acquired(*state, swapchain, *index);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                               uint32_t* index) {
    const auto state = FindDevice(device);
    if (!state || !state->vk.AcquireNextImage2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->vk.AcquireNextImage2KHR(device, info, index);
    if (state->preview && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) Acquired(*state, info->swapchain, *index);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    const uint64_t timestamp = framescope::QpcNow();
    const auto state = FindDevice(queue);
    if (!state || !state->vk.QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (!state->preview) return state->vk.QueuePresentKHR(queue, info);
    PreparedPresent prepared;
    try {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        prepared = PreparePresent(*state, queue, *info, timestamp);
    } catch (...) {
        Debug("preview preparation unavailable; original present preserved");
    }
    if (prepared.submit_result != VK_SUCCESS) return prepared.submit_result;
    VkPresentInfoKHR replacement = *info;
    VkSwapchainPresentFenceInfoEXT fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (prepared.submitted) {
        // Единственный consumer исходных waits — наш submit. Их нет в replacement.
        replacement.waitSemaphoreCount = 1;
        replacement.pWaitSemaphores = &prepared.semaphore;
        if (prepared.present_fence) {
            fence_info.pNext = info->pNext;
            fence_info.swapchainCount = 1;
            fence_info.pFences = &prepared.present_fence;
            replacement.pNext = &fence_info;
        }
    }
    // Не держим глобальный renderer mutex во время potentially blocking Win32 WSI.
    const VkResult result = state->vk.QueuePresentKHR(queue, prepared.submitted ? &replacement : info);
    try {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        if (result == VK_ERROR_DEVICE_LOST) state->lost = true;
        if (prepared.swap) {
            auto& swap = *prepared.swap;
            const uint32_t index = info->pImageIndices[0];
            if (PresentWasEnqueued(result) && index < swap.images.size()) swap.images[index].acquired = false;
            if (prepared.submitted) {
                if (prepared.present_fence) {
                    if (PresentWasEnqueued(result)) swap.present_fences.push_back(prepared.present_fence);
                    else state->vk.DestroyFence(state->device, prepared.present_fence, nullptr);
                } else swap.legacy_present_used = true;
                if (!PresentWasEnqueued(result) && index < swap.images.size()) swap.images[index].poisoned = true;
            }
            if (state->connected && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
                state->session.ObservePresent(timestamp, framescope::Api::Vulkan,
                    swap.extent.width, swap.extent.height, prepared.cpu_us);
        }
    } catch (...) { Debug("present completed; diagnostic publication failed"); }
    return result;
}

PFN_vkVoidFunction Interceptor(const char* name) noexcept {
#define FS_INTERCEPT(api, function) if (!std::strcmp(name, #api)) return reinterpret_cast<PFN_vkVoidFunction>(function)
    FS_INTERCEPT(vkGetInstanceProcAddr, vkGetInstanceProcAddr);
    FS_INTERCEPT(vkGetDeviceProcAddr, vkGetDeviceProcAddr);
    FS_INTERCEPT(vk_layerGetPhysicalDeviceProcAddr, vk_layerGetPhysicalDeviceProcAddr);
    FS_INTERCEPT(vkCreateInstance, CreateInstance);
    FS_INTERCEPT(vkDestroyInstance, DestroyInstance);
    FS_INTERCEPT(vkEnumerateInstanceLayerProperties, vkEnumerateInstanceLayerProperties);
    FS_INTERCEPT(vkEnumerateInstanceExtensionProperties, vkEnumerateInstanceExtensionProperties);
    FS_INTERCEPT(vkEnumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties);
    FS_INTERCEPT(vkEnumeratePhysicalDevices, EnumeratePhysicalDevices);
    FS_INTERCEPT(vkEnumeratePhysicalDeviceGroups, EnumeratePhysicalDeviceGroups);
    FS_INTERCEPT(vkEnumeratePhysicalDeviceGroupsKHR, EnumeratePhysicalDeviceGroups);
    FS_INTERCEPT(vkCreateDevice, CreateDevice);
    FS_INTERCEPT(vkDestroyDevice, DestroyDevice);
    FS_INTERCEPT(vkGetDeviceQueue, GetDeviceQueue);
    FS_INTERCEPT(vkGetDeviceQueue2, GetDeviceQueue2);
    FS_INTERCEPT(vkCreateSwapchainKHR, CreateSwapchain);
    FS_INTERCEPT(vkDestroySwapchainKHR, DestroySwapchain);
    FS_INTERCEPT(vkAcquireNextImageKHR, AcquireNextImage);
    FS_INTERCEPT(vkAcquireNextImage2KHR, AcquireNextImage2);
    FS_INTERCEPT(vkQueuePresentKHR, QueuePresent);
#undef FS_INTERCEPT
    return nullptr;
}

bool IsDeviceInterceptor(const char* name) noexcept {
    return !std::strcmp(name, "vkGetDeviceProcAddr") || !std::strcmp(name, "vkDestroyDevice") ||
           !std::strcmp(name, "vkGetDeviceQueue") || !std::strcmp(name, "vkGetDeviceQueue2") ||
           !std::strcmp(name, "vkCreateSwapchainKHR") || !std::strcmp(name, "vkDestroySwapchainKHR") ||
           !std::strcmp(name, "vkAcquireNextImageKHR") || !std::strcmp(name, "vkAcquireNextImage2KHR") ||
           !std::strcmp(name, "vkQueuePresentKHR");
}

} // namespace

// vk_layer.h объявляет этот entrypoint даже при VK_NO_PROTOTYPES. Clang считает
// добавление dllexport к последующему определению предупреждением, хотя ABI тот же.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
#endif
FS_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = vk_layerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (!instance) {
        if (!std::strcmp(name, "vkCreateInstance") || !std::strcmp(name, "vkGetInstanceProcAddr") ||
            !std::strcmp(name, "vkEnumerateInstanceLayerProperties") ||
            !std::strcmp(name, "vkEnumerateInstanceExtensionProperties") ||
            !std::strcmp(name, "vkCreateDevice")) return Interceptor(name);
        return nullptr;
    }
    const auto state = FindInstance(instance);
    if (!state) return nullptr;
    const auto downstream = state->next_gipa(instance, name);
    // В частности, не «включаем» отсутствующие extensions возвратом своего stub.
    if (!downstream) return nullptr;
    const auto intercept = Interceptor(name);
    return intercept ? intercept : downstream;
}

FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!device || !name) return nullptr;
    const auto state = FindDevice(device);
    if (!state) return nullptr;
    const auto downstream = state->next_gdpa(device, name);
    if (!downstream) return nullptr;
    if (IsDeviceInterceptor(name)) return Interceptor(name);
    return downstream;
}

FS_EXPORT PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    if (!instance || !name) return nullptr;
    const auto state = FindInstance(instance);
    if (!state) return nullptr;
    return state->next_phys_gpa ? state->next_phys_gpa(instance, name) : nullptr;
}

FS_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties* properties) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!properties) { *count = 1; return VK_SUCCESS; }
    if (*count == 0) return VK_INCOMPLETE;
    VkLayerProperties layer{};
    constexpr char description[] = "FrameScope preview HUD (FrameScope.Demo.exe only)";
    static_assert(sizeof(kLayerName) <= sizeof(layer.layerName));
    static_assert(sizeof(description) <= sizeof(layer.description));
    std::memcpy(layer.layerName, kLayerName, sizeof(kLayerName));
    std::memcpy(layer.description, description, sizeof(description));
    layer.specVersion = VK_API_VERSION_1_3;
    layer.implementationVersion = 1;
    properties[0] = layer;
    *count = 1;
    return VK_SUCCESS;
}

FS_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                                    VkExtensionProperties*) {
    if (!layer || std::strcmp(layer, kLayerName)) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}

FS_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char* layer,
    uint32_t* count, VkExtensionProperties* properties) {
    if (layer && !std::strcmp(layer, kLayerName)) {
        if (!count) return VK_ERROR_INITIALIZATION_FAILED;
        *count = 0;
        return VK_SUCCESS;
    }
    const auto state = FindPhysical(physical);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    return InstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(*state,
        "vkEnumerateDeviceExtensionProperties")(physical, layer, count, properties);
}

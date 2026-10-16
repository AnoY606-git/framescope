include(FetchContent)

# Архивы проверяются по SHA-256. Ни ветки, ни плавающие версии зависимостей не используются.
FetchContent_Declare(imgui
  URL https://codeload.github.com/ocornut/imgui/tar.gz/f5befd2d29e66809cd1110a152e375a7f1981f06
  URL_HASH SHA256=85f4ce357df05bcc331b587f01976f47fb55f19fadf477a7907289686bc3f4c8
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(minhook
  URL https://codeload.github.com/TsudaKageyu/minhook/tar.gz/c3fcafdc10146beb5919319d0683e44e3c30d537
  URL_HASH SHA256=31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(vulkan_headers
  URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/vulkan-sdk-1.4.363.0
  URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(level_zero_headers
  URL https://codeload.github.com/oneapi-src/level-zero/tar.gz/934714b278f850d164627eeaaa043eee02785a9e
  URL_HASH SHA256=ec16e5f6a21443080494fe9ebeae5ea36de003124211d58ae3e3ae5eccfd1940
  SOURCE_SUBDIR framescope_headers_only
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(imgui minhook vulkan_headers level_zero_headers)

add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}")

add_library(imgui_dx11 STATIC "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui_dx11 PUBLIC "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui_dx11 PUBLIC imgui d3d11 d3dcompiler)

add_library(imgui_dx12 STATIC "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
target_include_directories(imgui_dx12 PUBLIC "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui_dx12 PUBLIC imgui d3d12 dxgi d3dcompiler)

add_library(imgui_vulkan STATIC "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui_vulkan PUBLIC "${imgui_SOURCE_DIR}/backends" "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(imgui_vulkan PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(imgui_vulkan PUBLIC imgui)

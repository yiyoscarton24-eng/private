cmake_minimum_required(VERSION 3.21)
project(YiyoOverlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "YiyoOverlay solo se compila para Windows (DX11/DX12).")
endif()

include(FetchContent)

# 1. Fetch ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
)
FetchContent_MakeAvailable(imgui)

# 2. Fetch MinHook
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_MakeAvailable(minhook)

# Compilar ImGui
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# Compilar MinHook desde las fuentes
add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)

target_include_directories(minhook_static PUBLIC
    ${minhook_SOURCE_DIR}/include
)

# Compilar YiyoOverlay DLL
add_library(YiyoOverlay SHARED
    src/dllmain.cpp
    src/overlay.cpp
    src/metrics.cpp
    src/hook_dx11.cpp
    src/hook_dx12.cpp
    src/logger.cpp
    src/config.cpp
)

target_link_libraries(YiyoOverlay PRIVATE
    imgui
    minhook_static
    d3d11.lib
    d3d12.lib
    dxgi.lib
    dxguid.lib
    pdh.lib
    powrprof.lib
    user32.lib
    gdi32.lib
)

target_include_directories(YiyoOverlay PRIVATE 
    src
    ${minhook_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

set_target_properties(YiyoOverlay PROPERTIES
    OUTPUT_NAME "YiyoOverlay"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_custom_command(TARGET YiyoOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/config.ini
        $<TARGET_FILE_DIR:YiyoOverlay>/config.ini
)

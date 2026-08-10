cmake_minimum_required(VERSION 3.21)
project(YiyoOverlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "YiyoOverlay solo se compila para Windows (DX11/DX12).")
endif()

include(FetchContent)

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
)
FetchContent_MakeAvailable(imgui)

# MinHook
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_MakeAvailable(minhook)

# Librería estática para ImGui
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

# DLL de YiyoOverlay
add_library(YiyoOverlay SHARED
    src/dllmain.cpp
    src/overlay.cpp
    src/metrics.cpp
    src/hook_dx11.cpp
    src/hook_dx12.cpp
    src/logger.cpp
    src/config.cpp
)

# Se enlaza el target oficial de MinHook ('MinHook') en lugar de la cadena simple
target_link_libraries(YiyoOverlay PRIVATE
    imgui
    MinHook
    d3d11.lib
    d3d12.lib
    dxgi.lib
    dxguid.lib
    pdh.lib
    powrprof.lib
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

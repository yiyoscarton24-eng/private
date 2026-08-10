cmake_minimum_required(VERSION 3.21)

project(YiyoOverlay LANGUAGES C CXX)

if(NOT WIN32)
message(FATAL_ERROR "YiyoOverlay solo soporta Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$[CONFIG:Debug](CONFIG:Debug):Debug>")

```
add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

add_compile_options(
    /W3
    /EHsc
)
```

endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
set(YIYO_ARCH "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
set(YIYO_ARCH "x86")
else()
message(FATAL_ERROR "Arquitectura no soportada.")
endif()

message(STATUS "========================================")
message(STATUS "YiyoOverlay")
message(STATUS "Arquitectura: ${YIYO_ARCH}")
message(STATUS "========================================")

include(FetchContent)

FetchContent_Declare(
imgui
GIT_REPOSITORY https://github.com/ocornut/imgui.git
GIT_TAG v1.90.8
)

FetchContent_MakeAvailable(imgui)

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

FetchContent_Declare(
minhook
GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
GIT_TAG v1.3.3
)

FetchContent_MakeAvailable(minhook)

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

add_library(YiyoOverlay SHARED
src/dllmain.cpp
src/overlay.cpp
src/metrics.cpp
src/hook_dx11.cpp
src/hook_dx12.cpp
src/logger.cpp
src/config.cpp
)

target_include_directories(YiyoOverlay PRIVATE
${CMAKE_CURRENT_SOURCE_DIR}/src
${imgui_SOURCE_DIR}
${imgui_SOURCE_DIR}/backends
${minhook_SOURCE_DIR}/include
)

target_link_libraries(YiyoOverlay PRIVATE
imgui
minhook_static
d3d11
d3d12
dxgi
dxguid
pdh
powrprof
user32
gdi32
)

target_compile_definitions(YiyoOverlay PRIVATE
YIYOOVERLAY_BUILD
)

set_target_properties(YiyoOverlay PROPERTIES
OUTPUT_NAME "YiyoOverlay"
DEBUG_POSTFIX ""
PREFIX ""
)

foreach(CONFIG Debug Release RelWithDebInfo MinSizeRel)
string(TOUPPER "${CONFIG}" CONFIG_UPPER)

```
set_target_properties(YiyoOverlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        "${CMAKE_BINARY_DIR}/bin/${CONFIG}"
    LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        "${CMAKE_BINARY_DIR}/bin/${CONFIG}"
    ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        "${CMAKE_BINARY_DIR}/lib/${CONFIG}"
)
```

endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.ini")
add_custom_command(
TARGET YiyoOverlay
POST_BUILD
COMMAND ${CMAKE_COMMAND}
-E copy_if_different
"${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
"$<TARGET_FILE_DIR:YiyoOverlay>/config.ini"
COMMENT "Copiando config.ini"
)
endif()

message(STATUS "")
message(STATUS "========================================")
message(STATUS "YiyoOverlay configurado correctamente")
message(STATUS "Arquitectura: ${YIYO_ARCH}")
message(STATUS "DLL: YiyoOverlay.dll")
message(STATUS "========================================")
message(STATUS "")

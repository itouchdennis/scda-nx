# Linux and Nintendo Switch: one executable per image with the recompiled code linked in (no game code DLL, no
# compiler beside it), SDL2 for the window, input and sound, OpenGL for drawing. Included from CMakeLists.txt.
#
#   SCDA_GEN_DIR   generated code of default.xbe (default generated/game); a cross build (Switch) takes it from a
#                  native build or tools/regen.sh, which runs xbe_recomp on the host
#   SCDA_SWITCH    set by switch/build.sh: libnx platform layer, NRO
#   SCDA_CHECK_ONLY  CI: the port's own sources compiled, nothing linked and no game needed (tools/ci/check-build.sh)

option(SCDA_SWITCH "Nintendo Switch homebrew build" ${NINTENDO_SWITCH})
# char is signed on x86, where all of this was written, and unsigned on AArch64: keep x86's
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" OR SCDA_SWITCH)
    add_compile_options(-fsigned-char)
endif()
option(SCDA_VERBOSE_LOG "the debug build's full log in a release build" OFF)
option(SCDA_CHECK_ONLY "compile the port's own sources only: no recompiled code, no executable" OFF)
set(SCDA_GPU null CACHE STRING
    "renderer: vk (Vulkan 1.3 with shader objects), gl (OpenGL 4.3), or null (draws nothing, for testing the rest)")
# the Switch's Vulkan: mesa-switch's static NVK, installed as a portlibs tree (../mesa-build/build-linux.sh)
set(SCDA_NVK_SDK "" CACHE PATH "Switch, SCDA_GPU=vk: the Mesa Horizon SDK's portlibs/switch (lib/libvulkan.a)")
set(SCDA_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated/game CACHE PATH "generated code of default.xbe")
set(SCDA_PARTITIONS 128)

file(GLOB RECOMP_CONFIGS CONFIGURE_DEPENDS config/*.toml)

# x86 to C++ recompiler, a host tool
if(NOT CMAKE_CROSSCOMPILING)
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS tools/recomp/*.cpp)
    add_executable(xbe_recomp ${RECOMP_SOURCES})
    target_include_directories(xbe_recomp PRIVATE src tools/recomp)
    target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)
endif()

set(GEN ${SCDA_GEN_DIR})
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
math(EXPR last "${SCDA_PARTITIONS} - 1")
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
if(SCDA_CHECK_ONLY)
    if(NOT EXISTS ${GEN}/image_symbols.h)
        message(FATAL_ERROR "no image_symbols.h in ${GEN}: tools/ci/symbols_header.py makes it from the config")
    endif()
elseif(NOT CMAKE_CROSSCOMPILING)
    add_custom_command(
        OUTPUT ${GEN}/codegen.stamp
        BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h
        COMMAND xbe_recomp config/scda_v2.toml assets/default.xbe ${GEN}
        COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
        DEPENDS xbe_recomp ${RECOMP_CONFIGS} ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Recompiling default.xbe"
        VERBATIM)
    add_custom_target(recompile DEPENDS ${GEN}/codegen.stamp)
elseif(NOT EXISTS ${GEN}/image_symbols.h)
    message(FATAL_ERROR "no generated code in ${GEN}: run tools/regen.sh first")
endif()

# the recompiled code: strict aliasing off as on Windows, warnings off (generated), no frame pointer needed; its
# optimization another for measuring with -DSCDA_CODE_OPT (after the build type's, so it wins)
set(SCDA_CODE_OPT "-O2" CACHE STRING "optimization flags for the recompiled game code")
if(NOT SCDA_CHECK_ONLY)
    add_library(scda_code OBJECT ${GEN_SOURCES})
    target_include_directories(scda_code PRIVATE src ${GEN})
    separate_arguments(SCDA_CODE_OPT_LIST UNIX_COMMAND "${SCDA_CODE_OPT}")
    target_compile_options(scda_code PRIVATE ${SCDA_CODE_OPT_LIST} -fno-strict-aliasing -w)
    if(NOT CMAKE_CROSSCOMPILING)
        add_dependencies(scda_code recompile)
    endif()
endif()

# the host: what has no Windows parts as it is, the ported files and the port's own layer in src/port
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/(game|versus)/")
# Windows only: the D3D12 renderer and what serves it, XAudio2, the runtime compiler, splash and handover windows,
# the main window (src/port/main.cpp has the SDL one)
list(FILTER HOST_SOURCES EXCLUDE REGEX
    "/src/(gpu/(d3d12|pipeline_cache|texturedumper)|audio/(xaudio2|voice|voice_link)|app/(main|game_code|handover|splash|cursor|focus_pause))\\.cpp$")
# gyro.cpp reads SDL3's sensors, or libnx's on the Switch: elsewhere stubs.cpp answers for it
if(NOT SCDA_SWITCH)
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/core/gyro\\.cpp$")
endif()
if(SCDA_SWITCH)
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/port/linux_.*\\.cpp$")
else()
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/port/switch_.*\\.cpp$")
endif()
# gl: gpu_gl.cpp, with vk_shader.cpp's HLSL to SPIR-V that SPIRV-Cross turns into GLSL; vk: gpu_vk.cpp on vk_device.cpp,
# the SPIR-V as it is
if(SCDA_GPU STREQUAL "null")
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/port/(gpu_gl|gpu_vk|gpu_common|gl_|vk_).*\\.cpp$")
elseif(SCDA_GPU STREQUAL "gl")
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/port/(gpu_null|gpu_vk|vk_device)\\.cpp$")
elseif(SCDA_GPU STREQUAL "vk")
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/port/(gpu_null|gpu_gl|gl_.*)\\.cpp$")
else()
    message(FATAL_ERROR "SCDA_GPU is vk, gl or null, not ${SCDA_GPU}")
endif()
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
set(GPU_LIBS)
if(NOT SCDA_GPU STREQUAL "null")
    # built from source for both hosts, the Switch has no packages of them; third_party/ holds checkouts for builds
    # without network
    foreach(dep glslang SPIRV-Cross)
        string(TOUPPER ${dep} DEP)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt)
            set(FETCHCONTENT_SOURCE_DIR_${DEP} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep})
        endif()
    endforeach()
    FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 15.4.0
        GIT_SHALLOW TRUE)
    FetchContent_Declare(SPIRV-Cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG vulkan-sdk-1.4.321.0 GIT_SHALLOW TRUE)
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    foreach(off HLSL MSL CPP REFLECT C_API UTIL)
        set(SPIRV_CROSS_ENABLE_${off} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glslang SPIRV-Cross)
    set(GPU_LIBS glslang SPIRV glslang-default-resource-limits)
    if(SCDA_GPU STREQUAL "gl")
        list(APPEND GPU_LIBS spirv-cross-glsl spirv-cross-core)
    endif()
    if(SCDA_SWITCH AND SCDA_GPU STREQUAL "gl")
        # switch-sdl2's GL is EGL on switch-mesa (nouveau); the functions come from SDL_GL_GetProcAddress
        list(APPEND GPU_LIBS EGL glapi drm_nouveau)
        set(GPU_INCLUDES $ENV{DEVKITPRO}/portlibs/switch/include)
    elseif(SCDA_SWITCH)
        # NVK linked in, no loader; not find_package(Vulkan), its config wants libelf, which devkitPro has not:
        # switch_stubs.c stands in for it and for the EGL that libSDL2.a refers to. Listed twice for the archives to
        # be searched again
        if(NOT EXISTS ${SCDA_NVK_SDK}/lib/libvulkan.a)
            message(FATAL_ERROR "SCDA_NVK_SDK must hold lib/libvulkan.a (mesa-switch's Horizon SDK), not '${SCDA_NVK_SDK}'")
        endif()
        find_library(SCDA_ZSTD zstd PATHS $ENV{DEVKITPRO}/portlibs/switch/lib NO_DEFAULT_PATH)
        set(nvk ${SCDA_NVK_SDK}/lib/libvulkan.a expat $<$<BOOL:${SCDA_ZSTD}>:zstd> z nx)
        list(APPEND GPU_LIBS ${nvk} ${nvk} stdc++ m)
        set(GPU_INCLUDES ${SCDA_NVK_SDK}/include $ENV{DEVKITPRO}/portlibs/switch/include)
        list(APPEND HOST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/port/switch_stubs.c)
    else()
        find_package(Vulkan REQUIRED)
        list(APPEND GPU_LIBS Vulkan::Vulkan)
    endif()
endif()
set(SDL_VULKAN 0)

if(SCDA_CHECK_ONLY)
    add_library(scda OBJECT ${HOST_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES})
else()
    add_executable(scda ${HOST_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES} $<TARGET_OBJECTS:scda_code>)
endif()
target_include_directories(scda PRIVATE src src/port/include ${GEN})
target_include_directories(scda SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${GPU_INCLUDES})
target_compile_options(scda PRIVATE -fno-strict-aliasing -Wno-unused-parameter)
target_compile_definitions(scda PRIVATE SCDA_PORT $<$<BOOL:${SCDA_VERBOSE_LOG}>:SCDA_VERBOSE_LOG>
    $<$<STREQUAL:${SCDA_GPU},gl>:SCDA_GL> $<$<STREQUAL:${SCDA_GPU},vk>:SCDA_VK>
    $<$<AND:$<STREQUAL:${SCDA_GPU},vk>,$<NOT:$<BOOL:${SCDA_SWITCH}>>>:SCDA_SDL_VULKAN>)
target_link_libraries(scda PRIVATE tomlplusplus::tomlplusplus SDL2::SDL2 ${GPU_LIBS} Threads::Threads)
if(SCDA_SWITCH)
    target_compile_definitions(scda PRIVATE SCDA_SWITCH)
    target_link_options(scda PRIVATE "LINKER:--wrap=SDL_SYS_SetThreadPriority"
        "LINKER:--wrap=threadCreate" "LINKER:--wrap=threadClose"
        "LINKER:--wrap=condvarWaitTimeout"
        # the perf diagnostics' allocation sampling (switch_profile.cpp)
        "LINKER:--wrap=malloc" "LINKER:--wrap=calloc" "LINKER:--wrap=realloc" "LINKER:--wrap=memalign")
endif()
if(SCDA_SWITCH AND NOT SCDA_CHECK_ONLY)
    # the NRO for hbmenu: sdmc:/switch/scda/scda.nro
    set(SCDA_NACP ${CMAKE_CURRENT_BINARY_DIR}/scda.nacp)
    nx_generate_nacp(OUTPUT ${SCDA_NACP} NAME "Splinter Cell: Double Agent" AUTHOR "SCDA XBE Recomp"
        VERSION "${PROJECT_VERSION}")
    # your own 256x256 JPEG for hbmenu, kept out of git: switch/icon.jpg, or one given by path
    set(SCDA_ICON ${CMAKE_CURRENT_SOURCE_DIR}/switch/icon.jpg CACHE FILEPATH "the NRO's hbmenu icon, if it exists")
    if(EXISTS ${SCDA_ICON})
        nx_create_nro(scda NACP ${SCDA_NACP} ICON ${SCDA_ICON})
    else()
        nx_create_nro(scda NACP ${SCDA_NACP})
    endif()
endif()
# the xbe's SHA-1: the executable checks the game it finds is the release it was made from
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
    file(SHA1 ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe xbe_sha1)
else()
    set(xbe_sha1 "")
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/scda_xbe.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
target_sources(scda PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/scda_xbe.cpp)

# build_info.h, as on Windows
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
add_dependencies(scda build_info)
target_include_directories(scda PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

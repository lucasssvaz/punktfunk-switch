# disable in-source build
if (PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed")
endif ()

include(CMakeDependentOption)

# Platform
option(PLATFORM_DESKTOP "build for desktop (Linux)" OFF)
option(PLATFORM_SWITCH "build for switch" OFF)

set(_moonlight_enable_upscaling_default OFF)
if (PLATFORM_SWITCH)
    set(_moonlight_enable_upscaling_default ON)
endif ()
option(ENABLE_UPSCALING "Enable video upscaling on supported renderers" ${_moonlight_enable_upscaling_default})
unset(_moonlight_enable_upscaling_default)

option(MOONLIGHT_DESKTOP_USE_SYSTEM_PACKAGES "Use system-installed desktop dependencies (SDL2, FFmpeg, Opus, Jansson, libpng)" ON)
set(MOONLIGHT_DESKTOP_SYSTEM_PREFIX "" CACHE PATH "Optional prefix to prepend when resolving desktop system packages")
option(MOONLIGHT_DESKTOP_EMBED_RESOURCES "Embed desktop resources into the executable with libromfs" OFF)
option(MOONLIGHT_LINUX_REQUIRE_VAAPI "Fail Linux desktop configuration unless FFmpeg supports VA-API H.264 and HEVC decoding" OFF)

# Linux Only
cmake_dependent_option(INSTALL "Install to system." OFF "UNIX;NOT APPLE" OFF)

# Nintendo Switch Only
cmake_dependent_option(USE_DEKO3D "Using deko3d instead of OpenGL." OFF "PLATFORM_SWITCH" OFF)

option(USE_BOOST_FILESYSTEM "Whether to use the boost::filesystem instead of std::filesystem" OFF)

# Do not use it until you know what you are doing.
# You may need this option when packaging a AppImage.
option(CUSTOM_RESOURCES_DIR "Custom resources dir" "")

option(USE_GLFW "using glfw for input and create window" OFF)
option(USE_SDL2 "using sdl2 for input and create window" OFF)
option(USE_SDL3 "using sdl3 for input and create window" OFF)

# OpenGL version
option(USE_GL3 "using OpenGL 3.2+" OFF)
option(USE_GL2 "using OpenGL 2.1" OFF)
option(USE_GLES2 "using OpenGL ES 2.0" OFF)
option(USE_GLES3 "using OpenGL ES 3.0" OFF)
option(USE_DEKO3D "using Deko3D" OFF)

# If you do not want to attach necessary resource files when sending the program to others,
# or if you do not want others to modify the resource files, you can also enable this option
option(USE_LIBROMFS "using libromfs to bundle resources" OFF)

# Disable highlight border animation (useful for low-end devices)
option(SIMPLE_HIGHLIGHT "Simple highlight" OFF)

# Enable unity build, using -DCMAKE_UNITY_BUILD_BATCH_SIZE=8 to set the batch size
# https://cmake.org/cmake/help/latest/prop_tgt/UNITY_BUILD.html
option(BRLS_UNITY_BUILD "Unity build" OFF)


if (NOT DEFINED APP_PLATFORM_INCLUDE)
    set(APP_PLATFORM_INCLUDE)
endif ()

if (NOT DEFINED APP_PLATFORM_LIB)
    set(APP_PLATFORM_LIB)
endif ()

if (NOT DEFINED APP_PLATFORM_OPTION)
    set(APP_PLATFORM_OPTION)
endif ()

if (NOT DEFINED APP_PLATFORM_LINK_OPTION)
    set(APP_PLATFORM_LINK_OPTION)
    if (PLATFORM_SWITCH)
        list(APPEND APP_PLATFORM_LINK_OPTION "-Wl,-Map,borealis.map")
    endif ()
endif ()

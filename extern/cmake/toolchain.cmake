message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

function(check_libromfs_generator)
    if (LIBROMFS_PREBUILT_GENERATOR)
        if (NOT EXISTS "${LIBROMFS_PREBUILT_GENERATOR}")
            message(FATAL_ERROR "LIBROMFS_PREBUILT_GENERATOR does not exist: ${LIBROMFS_PREBUILT_GENERATOR}")
        endif ()
        return()
    endif ()

    # Native builds can build and execute libromfs-generator as a normal CMake
    # target. Only cross builds need a separate executable built for the host.
    # Do not automatically use a repository-root generator here: that file may
    # have been produced on a different operating system or architecture.
    if (CMAKE_CROSSCOMPILING)
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libromfs-generator")
            set(LIBROMFS_PREBUILT_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/libromfs-generator" PARENT_SCOPE)
        else ()
            message(FATAL_ERROR "Cross-compiling with libromfs requires a host LIBROMFS_PREBUILT_GENERATOR; refer to build_libromfs_generator.sh")
        endif ()
    endif()
endfunction()

if (PLATFORM_DESKTOP)
    add_definitions(-DPLATFORM_DESKTOP)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g2 -ggdb -Wall")
    set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
    if (NOT USE_GLFW AND NOT USE_SDL2 AND NOT USE_SDL3)
        set(USE_SDL3 ON)
    endif ()
    set(USE_GLES3 ON)

    if (NOT MOONLIGHT_DESKTOP_USE_SYSTEM_PACKAGES)
        message(FATAL_ERROR "Desktop builds require system SDL2, FFmpeg, Opus, Jansson and libpng; the vcpkg submodule was removed")
    endif ()

    if (MOONLIGHT_DESKTOP_EMBED_RESOURCES)
        set(USE_LIBROMFS ON)
        check_libromfs_generator()
    else ()
        set(USE_LIBROMFS OFF)
    endif ()

    set(_moonlight_system_prefix "${MOONLIGHT_DESKTOP_SYSTEM_PREFIX}")
    if (_moonlight_system_prefix STREQUAL "")
        if (DEFINED ENV{MINGW_PREFIX} AND NOT "$ENV{MINGW_PREFIX}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{MINGW_PREFIX}" _moonlight_system_prefix)
        elseif (DEFINED ENV{MSYSTEM_PREFIX} AND NOT "$ENV{MSYSTEM_PREFIX}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{MSYSTEM_PREFIX}" _moonlight_system_prefix)
        endif ()
    endif ()

    if (NOT _moonlight_system_prefix STREQUAL "")
        list(PREPEND CMAKE_PREFIX_PATH "${_moonlight_system_prefix}")
    endif ()

    message(STATUS "building for Desktop using system packages")
elseif (PLATFORM_SWITCH)
    add_definitions(-DPLATFORM_SWITCH)
    message(STATUS "building for SWITCH")
    # set(USE_SDL2 ON)
    set(USE_SDL3 OFF)
    set(USE_GLFW ON)
    set(USE_DEKO3D ON)

    if (NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
    endif ()
    set(DEVKITPRO $ENV{DEVKITPRO} CACHE BOOL "DEVKITPRO")
    set(__SWITCH__ ON)
    set(CMAKE_C_FLAGS "-I${DEVKITPRO}/libnx/include -I${DEVKITPRO}/portlibs/switch/include")
    set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}")
    include(${DEVKITPRO}/cmake/Switch.cmake REQUIRED)
else()
    message(FATAL_ERROR "Please set build target. Example: -DPLATFORM_DESKTOP=ON or -DPLATFORM_SWITCH=ON")
endif ()

# OpenGL driver
if (USE_DEKO3D)
    message(STATUS "USE_DEKO3D")
    set(BOREALIS_USE_DEKO3D ON)
elseif (USE_GL2)
    message(STATUS "USE_GL2")
    add_definitions(-DUSE_GL2)
elseif (USE_GLES2)
    message(STATUS "USE_GLES2")
    add_definitions(-DUSE_GLES2)
elseif (USE_GLES3)
    message(STATUS "USE_GLES3")
    add_definitions(-DUSE_GLES3)
else ()
    message(STATUS "USE_GL3/4")
endif ()

# SDL or GLFW
if (USE_SDL3)
    message(STATUS "SDL3")
    set(USE_SDL2 OFF)
    set(USE_GLFW OFF)
    add_definitions(-D__SDL3__)
elseif (USE_SDL2)
    message(STATUS "SDL2")
    set(USE_SDL2 ON)
    set(USE_GLFW OFF)
    add_definitions(-D__SDL2__)
else ()
    message(STATUS "GLFW")
    set(USE_SDL2 OFF)
    set(USE_GLFW ON)
    add_definitions(-D__GLFW__)
endif ()

if (USE_EGL)
    message(STATUS "Using EGL")
    add_definitions(-DUSE_EGL)
elseif (USE_DEKO3D)
    message(STATUS "Using Deko3D")
else ()
    message(STATUS "Using native GL")
endif ()
# todo: Remove this once https://github.com/xfangfang/wiliwili/pull/173 is merged
function(main_target var1 var2 var3)
    add_executable(${var1} ${var2})
endfunction()

function(program_target target source)
    add_executable(${target} ${source})
endfunction()

function(linux_install res res_install svg_icon icon_res_dir icon_res_list appdata desktop)
    message("install prefix: ${CMAKE_INSTALL_PREFIX}")
    # bin
    install(TARGETS ${PROJECT_NAME}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
    # resources
    install(DIRECTORY ${res}
            DESTINATION ${res_install}
            FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # svg icon
    install(FILES ${svg_icon}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # png icon
    foreach (icon_dir ${icon_res_list})
        install(FILES ${icon_res_dir}/${icon_dir}/${PACKAGE_NAME}.png
                DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/${icon_dir}/apps
                PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endforeach ()
    # appdata
    install(FILES ${appdata}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/share/metainfo
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # desktop
    install(FILES ${desktop} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)
endfunction()

function(add_libromfs app res)
    message(STATUS "USE LIBROMFS")
    add_definitions(-DUSE_LIBROMFS)
    set(LIBROMFS_PROJECT_NAME ${app} PARENT_SCOPE)
    set(LIBROMFS_RESOURCE_LOCATION "${res}" PARENT_SCOPE)
endfunction()

function(git_info tag short)
    # Add git info
    find_package(Git)
    if(GIT_EXECUTABLE)
        execute_process(COMMAND git describe --tags
                TIMEOUT 5
                OUTPUT_VARIABLE GIT_TAG_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
        execute_process(COMMAND git rev-parse --short HEAD
                TIMEOUT 5
                OUTPUT_VARIABLE GIT_TAG_SHORT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
        message(STATUS "building from git tag: ${GIT_TAG_VERSION}")
        message(STATUS "building from git commit: ${GIT_TAG_SHORT}")
    else ()
        set(GIT_TAG_VERSION "")
        set(GIT_TAG_SHORT "")
    endif ()
    set(${tag} ${GIT_TAG_VERSION} PARENT_SCOPE)
    set(${short} ${GIT_TAG_SHORT} PARENT_SCOPE)
endfunction()

# switch platform
# glsl -> deko3d dksh
function(gen_dksh romfs_dir)
    find_program(NX_UAM_EXE NAMES uam HINTS "${DEVKITPRO}/tools/bin")
    if (NOT NX_UAM_EXE)
        message(FATAL_ERROR "Could not find uam: try installing uam")
    endif()
    if (NOT DEFINED BOREALIS_LIBRARY)
        message(FATAL_ERROR BOREALIS_LIBRARY is not defined)
    endif ()

    set(NANOVG_SHADER_BASE "${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders")

    file(MAKE_DIRECTORY "${romfs_dir}")
    execute_process(COMMAND ${NX_UAM_EXE} -s vert -o "${romfs_dir}/fill_vsh.dksh" "${NANOVG_SHADER_BASE}/fill_vsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_aa_fsh.dksh" "${NANOVG_SHADER_BASE}/fill_aa_fsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_fsh.dksh" "${NANOVG_SHADER_BASE}/fill_fsh.glsl" TIMEOUT 5)

    execute_process(COMMAND ${NX_UAM_EXE} -s vert -o "${romfs_dir}/basic_vsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/basic_vsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/color_fsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/color_fsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/texture_fsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/texture_fsh.glsl" TIMEOUT 5)
    if (ENABLE_UPSCALING)
        execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/upscaling_fsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/upscaling_fsh.glsl" TIMEOUT 5)
        execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/rcas_fsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/rcas_fsh.glsl" TIMEOUT 5)
        execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/upscaling_pass_fsh.dksh" "${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/video/deko3d/upscaling_pass_fsh.glsl" TIMEOUT 5)
    endif ()
endfunction()

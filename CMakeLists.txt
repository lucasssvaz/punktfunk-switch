# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.10)

set(EXTERN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/extern)
set(BOREALIS_LIBRARY ${EXTERN_PATH}/borealis/library)

# build options
include(${EXTERN_PATH}/cmake/commonOption.cmake)

# Dependencies
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)

# toolchain
include(${EXTERN_PATH}/cmake/toolchain.cmake)

# Renderers
if (PLATFORM_SWITCH)
    set(USE_DEKO3D_RENDERER ON)
else ()
    set(USE_GL_RENDERER ON)
endif ()

set(SUPPORT_UPSCALING OFF)
if (ENABLE_UPSCALING)
    message(STATUS "Use ENABLE_UPSCALING")
    if (PLATFORM_SWITCH AND USE_DEKO3D_RENDERER)
        set(SUPPORT_UPSCALING ON)
    else ()
        message(FATAL_ERROR "ENABLE_UPSCALING=ON is only supported with the Switch deko3d renderer")
    endif ()
endif ()

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "${EXTERN_PATH}/cmake")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${EXTERN_PATH}/cmake")

# project info
project(punktfunk)
include(GNUInstallDirs)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")
set(VERSION_ALTER "0")
set(VERSION_BUILD "1")
set(PACKAGE_NAME "io.punktfunk.switch")
set(PROJECT_AUTHOR "Lucas Saavedra Vaz")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/img/punktfunk_icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

if (PLATFORM_DESKTOP AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT USE_LIBROMFS)
    # Borealis appends "/resources/" to this prefix. This keeps non-embedded
    # installed builds FHS-compliant instead of depending on their working
    # directory.
    set(BRLS_RESOURCES_DIR "${CMAKE_INSTALL_FULL_DATADIR}/punktfunk-switch"
        CACHE STRING "Installed Borealis resource prefix" FORCE)
endif ()

add_definitions(
        -DAPP_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
        -Dcimg_display=0
        -Dcimg_use_png
        -Dcimg_no_system_calls
        -DPUNKTFUNK_FEATURE_QUIC
        -DBRLS_I18N_PREFIX=\"main/\")

if (USE_LIBROMFS)
    add_libromfs(${PROJECT_NAME} ${PROJECT_RESOURCES})
endif ()

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
execute_process(
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-patched-crates.sh
    RESULT_VARIABLE _pf_fetch_result)
if (NOT _pf_fetch_result EQUAL 0)
    message(FATAL_ERROR "scripts/fetch-patched-crates.sh failed (${_pf_fetch_result})")
endif ()
add_subdirectory(extern)

# setting src and include
file(GLOB MAIN_SRC CONFIGURE_DEPENDS
    app/src/*.cpp
    app/src/gestures/*.cpp
    app/src/keyboards/*.cpp
    app/src/punktfunk/*.cpp
    app/src/streaming/*.cpp
    app/src/streaming/audio/*.cpp
    app/src/streaming/ffmpeg/*.cpp
    app/src/streaming/switch/*.cpp
    app/src/views/*.cpp
    app/src/utils/*.cpp)

if (PLATFORM_SWITCH)
    list(REMOVE_ITEM MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/app/src/streaming/audio/SDLAudioRenderer.cpp)
    set(PLATFORM_LIBS
        # base lib
        nx m
    )
    list(APPEND MAIN_SRC
            ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c
            app/src/switch/wrapper.c
            app/src/switch/thread_affinity.c
            # libc gaps the Rust std port expects (getrandom, sysconf, getifaddrs, ...)
            punktfunk-rs/shims/horizon_shims.c
            )
endif ()

if (USE_DEKO3D_RENDERER)
    list(APPEND MAIN_SRC app/src/streaming/video/deko3d/DKVideoRenderer.cpp)
elseif (USE_GL_RENDERER)
    list(APPEND MAIN_SRC app/src/streaming/video/OpenGL/GLVideoRenderer.cpp)
endif ()

# building target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)
if (USE_SDL3)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SDL_ENABLE_OLD_NAMES)
endif ()

# punktfunk core: punktfunk-ffi (QUIC connection API) + the mgmt-API client, one Rust staticlib.
# The Switch build needs nightly cargo (-Zbuild-std on the custom target, see
# punktfunk-rs/.cargo/config.toml) and runs inside the pf-switch-dev container.
set(PF_RS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/punktfunk-rs)
set(PF_CARGO "cargo" CACHE STRING "cargo executable used to build libpf_switch.a")
set(PF_RUST_LIB "" CACHE FILEPATH "Prebuilt libpf_switch.a (skips the cargo step when set)")
if (PF_RUST_LIB STREQUAL "")
    if (PLATFORM_SWITCH)
        set(_pf_rust_triple aarch64-nintendo-switch-libnx)
        set(_pf_cargo_target_args)
    else ()
        execute_process(COMMAND rustc -vV OUTPUT_VARIABLE _pf_rustc_vv)
        string(REGEX MATCH "host: ([^\n]+)" _ "${_pf_rustc_vv}")
        set(_pf_rust_triple ${CMAKE_MATCH_1})
        set(_pf_cargo_target_args --target ${_pf_rust_triple})
    endif ()
    set(_pf_rust_lib ${PF_RS_DIR}/target/${_pf_rust_triple}/release/libpf_switch.a)
    add_custom_target(pf_switch_rs
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-patched-crates.sh
        COMMAND ${PF_CARGO} build --release -p pf-switch ${_pf_cargo_target_args}
        WORKING_DIRECTORY ${PF_RS_DIR}
        BYPRODUCTS ${_pf_rust_lib}
        USES_TERMINAL
        COMMENT "cargo build -p pf-switch (${_pf_rust_triple})")
    add_dependencies(${PROJECT_NAME} pf_switch_rs)
else ()
    set(_pf_rust_lib ${PF_RUST_LIB})
endif ()

# building release file
if (PLATFORM_DESKTOP)
    add_custom_target(${PROJECT_NAME}.data
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_CURRENT_BINARY_DIR}/resources
    )

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # linuxdeploy and the portable tarball put private dependencies in
        # ../lib relative to the executable. Native packages can leave that
        # directory empty and continue using system libraries.
        set_target_properties(${PROJECT_NAME} PROPERTIES
            INSTALL_RPATH "\$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH OFF)
    endif ()

    if (NOT USE_LIBROMFS)
        add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
    endif ()
elseif (PLATFORM_SWITCH)
    if (USE_DEKO3D)
        gen_dksh("${PROJECT_RESOURCES}/shaders")
    endif ()
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "PFSW" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
    list(APPEND APP_PLATFORM_LIB dav1d z)
endif ()

target_include_directories(${PROJECT_NAME} PUBLIC
        app/include
        app/include/gestures
        app/src/gestures
        app/src/keyboards
        app/src/punktfunk
        app/src/streaming
        app/src/streaming/audio
        app/src/streaming/ffmpeg
        app/src/streaming/switch
        app/src/streaming/video
        app/src/streaming/video/deko3d
        app/src/streaming/video/OpenGL
        app/src/utils
        extern/CImg
        extern/punktfunk/include
        punktfunk-rs/pf-switch/include
        ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-ffunction-sections>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fdata-sections>
        ${APP_PLATFORM_OPTION})

if(PLATFORM_SWITCH)
    target_include_directories(${PROJECT_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/switch/include
    )
    set_source_files_properties(punktfunk-rs/shims/horizon_shims.c PROPERTIES
            INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/punktfunk-rs/shims/include)
endif ()

set(MOONLIGHT_APP_SDL_LIBS)
if (USE_SDL3)
    list(APPEND MOONLIGHT_APP_SDL_LIBS SDL3::SDL3)
elseif (USE_SDL2)
    if (TARGET SDL2::SDL2)
        list(APPEND MOONLIGHT_APP_SDL_LIBS SDL2::SDL2)
    elseif (TARGET SDL2::SDL2-static)
        list(APPEND MOONLIGHT_APP_SDL_LIBS SDL2::SDL2-static)
    else ()
        find_package(SDL2 REQUIRED)
        if (TARGET SDL2::SDL2)
            list(APPEND MOONLIGHT_APP_SDL_LIBS SDL2::SDL2)
        elseif (TARGET SDL2::SDL2-static)
            list(APPEND MOONLIGHT_APP_SDL_LIBS SDL2::SDL2-static)
        elseif (SDL2_LIBRARIES)
            list(APPEND MOONLIGHT_APP_SDL_LIBS ${SDL2_LIBRARIES})
        else ()
            message(FATAL_ERROR "SDL2 requested but no SDL2 target was found")
        endif ()
    endif ()
endif ()

set(MOONLIGHT_APP_FFMPEG_LIBS)
set(MOONLIGHT_APP_MISC_LIBS)
find_package(Jansson REQUIRED)
find_package(PNG REQUIRED)

find_library(OPUS_LIBRARY opus)
message("opus: ${OPUS_LIBRARY}")

if(PLATFORM_SWITCH)
    list(APPEND MOONLIGHT_APP_FFMPEG_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/switch/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/switch/libavformat.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/switch/libavutil.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/switch/libswresample.a)
else()
    find_library(AVCODEC_LIBRARY avcodec)
    message("avcodec: ${AVCODEC_LIBRARY}")

    find_library(AVFORMAT_LIBRARY avformat)
    message("avformat: ${AVFORMAT_LIBRARY}")

    find_library(AVUTIL_LIBRARY avutil)
    message("avutil: ${AVUTIL_LIBRARY}")

    find_library(SWRESAMPLE_LIBRARY swresample)
    message("swresample: ${SWRESAMPLE_LIBRARY}")

    get_filename_component(_moonlight_ffmpeg_library_dir
        "${AVCODEC_LIBRARY}" DIRECTORY)
    get_filename_component(_moonlight_ffmpeg_prefix
        "${_moonlight_ffmpeg_library_dir}/../.." ABSOLUTE)
    find_path(MOONLIGHT_FFMPEG_INCLUDE_DIR
        NAMES libavcodec/avcodec.h
        HINTS "${_moonlight_ffmpeg_prefix}/include"
        PATH_SUFFIXES "${CMAKE_LIBRARY_ARCHITECTURE}")
    if (NOT MOONLIGHT_FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR "Could not locate the FFmpeg development headers")
    endif ()
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${MOONLIGHT_FFMPEG_INCLUDE_DIR}")

    list(APPEND MOONLIGHT_APP_FFMPEG_LIBS
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWRESAMPLE_LIBRARY})

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
        PLATFORM_DESKTOP AND MOONLIGHT_LINUX_REQUIRE_VAAPI)
        include(CheckCSourceRuns)
        set(_moonlight_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
        set(_moonlight_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
        set(CMAKE_REQUIRED_LIBRARIES
            "${AVCODEC_LIBRARY};${AVUTIL_LIBRARY}")
        set(CMAKE_REQUIRED_INCLUDES "${MOONLIGHT_FFMPEG_INCLUDE_DIR}")
        unset(MOONLIGHT_FFMPEG_HAS_REQUIRED_VAAPI CACHE)
        check_c_source_runs([=[
            #include <libavcodec/avcodec.h>
            #include <libavutil/hwcontext.h>

            static int codec_supports_vaapi(enum AVCodecID id) {
                const AVCodec *codec = avcodec_find_decoder(id);
                if (codec == 0) {
                    return 0;
                }

                for (int index = 0; ; ++index) {
                    const AVCodecHWConfig *config =
                        avcodec_get_hw_config(codec, index);
                    if (config == 0) {
                        return 0;
                    }
                    if (config->device_type == AV_HWDEVICE_TYPE_VAAPI &&
                        (config->methods &
                         AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                        return 1;
                    }
                }
            }

            int main(void) {
                return codec_supports_vaapi(AV_CODEC_ID_H264) &&
                               codec_supports_vaapi(AV_CODEC_ID_HEVC)
                           ? 0
                           : 1;
            }
        ]=] MOONLIGHT_FFMPEG_HAS_REQUIRED_VAAPI)
        set(CMAKE_REQUIRED_LIBRARIES "${_moonlight_saved_required_libraries}")
        set(CMAKE_REQUIRED_INCLUDES "${_moonlight_saved_required_includes}")
        unset(_moonlight_saved_required_libraries)
        unset(_moonlight_saved_required_includes)

        if (NOT MOONLIGHT_FFMPEG_HAS_REQUIRED_VAAPI)
            message(FATAL_ERROR
                "The selected FFmpeg libraries do not expose VA-API "
                "hardware configurations for both H.264 and HEVC. "
                "Install a VA-API-enabled FFmpeg build or configure with "
                "-DMOONLIGHT_LINUX_REQUIRE_VAAPI=OFF for a developer-only "
                "software-decoding build.")
        endif ()
    endif ()
    unset(_moonlight_ffmpeg_library_dir)
    unset(_moonlight_ffmpeg_prefix)
endif ()

list(APPEND MOONLIGHT_APP_MISC_LIBS
    Jansson::Jansson
    PNG::PNG)

# Rust staticlibs do not carry their native dependencies: punktfunk-core decodes Opus in-core.
set(PF_RUST_NATIVE_LIBS ${OPUS_LIBRARY})
if (PLATFORM_SWITCH)
    list(APPEND PF_RUST_NATIVE_LIBS nx m)
else ()
    list(APPEND PF_RUST_NATIVE_LIBS pthread dl m)
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
        borealis
        mdns
        ${MOONLIGHT_APP_MISC_LIBS}
        ${MOONLIGHT_APP_FFMPEG_LIBS}
        ${MOONLIGHT_APP_SDL_LIBS}
        ${APP_PLATFORM_LIB}
        ${_pf_rust_lib}
        ${PF_RUST_NATIVE_LIBS})
if (PLATFORM_SWITCH)
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--wrap=pthread_create
        -Wl,--wrap=pthread_join
        -Wl,--wrap=pthread_detach)
endif ()

if (USE_GL_RENDERER)
    add_definitions(-DUSE_GL_RENDERER)
elseif (USE_DEKO3D_RENDERER)
    add_definitions(-DUSE_DEKO3D_RENDERER)
endif ()

if (SUPPORT_UPSCALING)
    add_definitions(-DSUPPORT_UPSCALING)
endif ()

if (PLATFORM_DESKTOP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Runtime)

    if (NOT USE_LIBROMFS)
        install(DIRECTORY "${PROJECT_RESOURCES}/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/punktfunk-switch/resources"
            COMPONENT Runtime)
    endif ()

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/punktfunk-switch"
        COMPONENT Runtime)
endif ()

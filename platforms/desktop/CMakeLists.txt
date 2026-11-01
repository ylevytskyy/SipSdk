cmake_minimum_required(VERSION 3.15)
project(sip_desktop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_definitions(USE_UNIXSOCK=ON)

# Detect host desktop slice
if(APPLE)
    set(DESKTOP_OS "darwin")
else()
    set(DESKTOP_OS "linux")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(DESKTOP_ARCH "arm64")
else()
    set(DESKTOP_ARCH "x86_64")
endif()

set(DESKTOP_SLICE "${DESKTOP_OS}-${DESKTOP_ARCH}")
set(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/${DESKTOP_SLICE}")
set(PREBUILT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/include")

if(NOT EXISTS "${PREBUILT_DIR}/lib/libbaresip.a" OR NOT EXISTS "${PREBUILT_DIR}/lib/libre.a")
    message(STATUS "Desktop prebuilts not found for ${DESKTOP_SLICE}, executing build-baresip-desktop.sh...")
    execute_process(
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build-baresip-desktop.sh" "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt"
        RESULT_VARIABLE BUILD_RES
    )
    if(NOT BUILD_RES EQUAL 0)
        message(FATAL_ERROR "Failed to build Baresip desktop prebuilts for ${DESKTOP_SLICE}")
    endif()
endif()

set(BARESIP_LIB "${PREBUILT_DIR}/lib/libbaresip.a")
set(RE_LIB "${PREBUILT_DIR}/lib/libre.a")

# Include core
set(BARESIP_INCLUDE_DIR "${PREBUILT_INCLUDE_DIR};${PREBUILT_INCLUDE_DIR}/re;${PREBUILT_INCLUDE_DIR}/baresip" CACHE PATH "Path to Baresip headers" FORCE)
set(BARESIP_LIBRARY "${BARESIP_LIB}" CACHE FILEPATH "Path to Baresip library" FORCE)

if(NOT TARGET sip_core)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../core" "${CMAKE_CURRENT_BINARY_DIR}/core")
endif()

# Desktop SDK static library
set(SIP_DESKTOP_SOURCES
    src/call.cpp
    src/client.cpp
    src/audio_driver.cpp
    src/v4l2_capture.cpp
)

if(APPLE)
    list(APPEND SIP_DESKTOP_SOURCES src/avfoundation_capture.mm)
endif()

add_library(sip_desktop STATIC
    ${SIP_DESKTOP_SOURCES}
)

set_target_properties(sip_desktop PROPERTIES
    OUTPUT_NAME "sip_desktop"
)

target_include_directories(sip_desktop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
        ${PREBUILT_INCLUDE_DIR}
        ${PREBUILT_INCLUDE_DIR}/re
        ${PREBUILT_INCLUDE_DIR}/baresip
)

if(APPLE)
    target_link_libraries(sip_desktop
        PUBLIC
            sip_core
            ${BARESIP_LIB}
            ${RE_LIB}
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework SystemConfiguration"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework CFNetwork"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
            z
            m
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(sip_desktop
        PUBLIC
            -Wl,--start-group
            sip_core
            ${BARESIP_LIB}
            ${RE_LIB}
            -Wl,--end-group
            Threads::Threads
            asound
            pulse
            dl
            m
            z
    )
endif()

add_library(sip::desktop ALIAS sip_desktop)

# Sample application
add_subdirectory(sample)

# Standalone Headless CLI tool
add_executable(sip_cli
    src/cli/main.cpp
)

target_link_libraries(sip_cli
    PRIVATE
        sip::desktop
)

target_include_directories(sip_cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
        ${PREBUILT_INCLUDE_DIR}
)

# Unit & lifecycle tests
enable_testing()

add_executable(test_desktop_lifecycle
    tests/test_desktop_lifecycle.cpp
)

target_link_libraries(test_desktop_lifecycle
    PRIVATE
        sip::desktop
)

target_include_directories(test_desktop_lifecycle
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
        ${PREBUILT_INCLUDE_DIR}
)

add_test(NAME desktop_lifecycle COMMAND test_desktop_lifecycle)

add_executable(test_audio_drivers
    tests/test_audio_drivers.cpp
)

target_link_libraries(test_audio_drivers
    PRIVATE
        sip::desktop
)

target_include_directories(test_audio_drivers
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
        ${PREBUILT_INCLUDE_DIR}
)

add_test(NAME desktop_audio_drivers COMMAND test_audio_drivers)

add_executable(test_video_capture
    tests/test_video_capture.cpp
)

target_link_libraries(test_video_capture
    PRIVATE
        sip::desktop
)

target_include_directories(test_video_capture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
        ${PREBUILT_INCLUDE_DIR}
)

add_test(NAME desktop_video_capture COMMAND test_video_capture)

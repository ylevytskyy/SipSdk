cmake_minimum_required(VERSION 3.15)
project(sip_windows LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(WIN32 OR MINGW)
    add_compile_definitions(
        WIN32=1
        _WIN32=1
        USE_UNIXSOCK=ON
        NOMINMAX=1
        WIN32_LEAN_AND_MEAN=1
        _WIN32_WINNT=0x0601
    )
else()
    add_compile_definitions(
        USE_UNIXSOCK=ON
    )
endif()

set(PREBUILT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../desktop/prebuilt/include")
set(BARESIP_INCLUDE_DIR "${PREBUILT_INCLUDE_DIR};${PREBUILT_INCLUDE_DIR}/re;${PREBUILT_INCLUDE_DIR}/baresip" CACHE PATH "Path to Baresip headers" FORCE)

# Set up core library paths
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
set(BARESIP_LIBRARY "" CACHE FILEPATH "Path to Baresip library" FORCE)

# Check Windows prebuilts
set(WIN_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/windows-x86_64")
set(BARESIP_LIB "")
set(RE_LIB "")

if(WIN32 OR MINGW)
    if(NOT EXISTS "${WIN_PREBUILT_DIR}/lib/libbaresip.a" OR NOT EXISTS "${WIN_PREBUILT_DIR}/lib/libre.a")
        message(STATUS "Windows prebuilts not found, executing build-baresip-windows.sh...")
        execute_process(
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build-baresip-windows.sh" "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt"
            RESULT_VARIABLE BUILD_RES
        )
    endif()

    if(EXISTS "${WIN_PREBUILT_DIR}/lib/libbaresip.a")
        set(BARESIP_LIB "${WIN_PREBUILT_DIR}/lib/libbaresip.a")
        set(RE_LIB "${WIN_PREBUILT_DIR}/lib/libre.a")
    endif()
endif()

# Build sip_core shared library (libsipcore.dll)
add_library(sip_core_dll SHARED
    ${CORE_DIR}/src/call_session.cpp
    ${CORE_DIR}/src/engine.cpp
    ${CORE_DIR}/src/media_driver.cpp
    ${CORE_DIR}/src/sip_abi.cpp
    src/media/mf_capture.cpp
    src/media/mf_codec.cpp
    src/media/mf_renderer.cpp
)

set_target_properties(sip_core_dll PROPERTIES
    OUTPUT_NAME "sipcore"
    PREFIX "lib"
)

target_include_directories(sip_core_dll
    PUBLIC
        $<BUILD_INTERFACE:${CORE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CORE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PREBUILT_INCLUDE_DIR}
        ${PREBUILT_INCLUDE_DIR}/re
        ${PREBUILT_INCLUDE_DIR}/baresip
)

target_compile_definitions(sip_core_dll
    PRIVATE
        SIP_CORE_BUILDING_LIB=1
        SIP_CORE_BUILDING_DLL=1
)

if(WIN32 OR MINGW)
    target_link_libraries(sip_core_dll
        PRIVATE
            ${BARESIP_LIB}
            ${RE_LIB}
            ws2_32
            iphlpapi
            mfplat
            mfuuid
            mfreadwrite
            strmiids
            d3d11
            dxgi
            shlwapi
    )
elseif(APPLE)
    # Host Darwin verification configuration
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(HOST_ARCH "arm64")
    else()
        set(HOST_ARCH "x86_64")
    endif()
    set(DESKTOP_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../desktop/prebuilt/darwin-${HOST_ARCH}")
    set(BARESIP_LIB "${DESKTOP_PREBUILT_DIR}/lib/libbaresip.a")
    set(RE_LIB "${DESKTOP_PREBUILT_DIR}/lib/libre.a")

    target_link_libraries(sip_core_dll
        PRIVATE
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
            pthread
    )
else()
    # Non-Windows / Linux cross/host compilation fallback for testing ABI
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(LINUX_ARCH "arm64")
    else()
        set(LINUX_ARCH "x86_64")
    endif()
    set(DESKTOP_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../desktop/prebuilt/linux-${LINUX_ARCH}")
    set(BARESIP_LIB "${DESKTOP_PREBUILT_DIR}/lib/libbaresip.a")
    set(RE_LIB "${DESKTOP_PREBUILT_DIR}/lib/libre.a")

    target_link_libraries(sip_core_dll
        PRIVATE
            ${BARESIP_LIB}
            ${RE_LIB}
            asound
            pulse
            dl
            m
            z
            pthread
    )
endif()

# ABI test executable
add_executable(test_windows_abi
    tests/test_windows_abi.cpp
)

target_include_directories(test_windows_abi
    PRIVATE
        ${CORE_DIR}/include
)

target_link_libraries(test_windows_abi
    PRIVATE
        sip_core_dll
)

if(NOT WIN32 AND NOT MINGW)
    target_link_libraries(test_windows_abi PRIVATE dl pthread)
endif()

# Windows Media Foundation drivers test executable
add_executable(test_mf_drivers
    tests/test_mf_drivers.cpp
    src/media/mf_capture.cpp
    src/media/mf_codec.cpp
    src/media/mf_renderer.cpp
)

target_include_directories(test_mf_drivers
    PRIVATE
        ${CORE_DIR}/include
        include
)

target_link_libraries(test_mf_drivers
    PRIVATE
        sip_core_dll
        pthread
)


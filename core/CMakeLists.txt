cmake_minimum_required(VERSION 3.15)
project(sip_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_definitions(USE_UNIXSOCK=ON)

# Baresip dependencies
if(NOT BARESIP_INCLUDE_DIR)
    message(FATAL_ERROR "BARESIP_INCLUDE_DIR must be specified (path to Baresip & libre headers)")
endif()

# Core static library
add_library(sip_core STATIC
    src/engine.cpp
    src/sip_abi.cpp
)

set_target_properties(sip_core PROPERTIES
    OUTPUT_NAME "sipcore"
)

target_include_directories(sip_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${BARESIP_INCLUDE_DIR}
)

target_compile_definitions(sip_core
    PRIVATE
        SIP_CORE_BUILDING_LIB=1
)

# Test runner
if(BARESIP_LIBRARY)
    add_executable(sip_core_lifecycle_test
        tests/test_lifecycle.cpp
    )

    target_include_directories(sip_core_lifecycle_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(sip_core_lifecycle_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    if(APPLE)
        target_link_libraries(sip_core_lifecycle_test
            PRIVATE
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
    endif()
endif()

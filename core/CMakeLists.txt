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
add_compile_definitions(USE_TLS=1)
add_compile_definitions(USE_DTLS=1)
add_compile_definitions(USE_OPENSSL=1)

# Baresip dependencies
if(NOT BARESIP_INCLUDE_DIR)
    message(FATAL_ERROR "BARESIP_INCLUDE_DIR must be specified (path to Baresip & libre headers)")
endif()

# Core static library
add_library(sip_core STATIC
    src/account_session.cpp
    src/call_recorder.cpp
    src/call_session.cpp
    src/conference_mixer.cpp
    src/engine.cpp
    src/media_driver.cpp
    src/mkv_writer.cpp
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

# Core shared library (Windows DLL, macOS dylib, Linux so)
add_library(sip_core_shared SHARED
    src/account_session.cpp
    src/call_recorder.cpp
    src/call_session.cpp
    src/conference_mixer.cpp
    src/engine.cpp
    src/media_driver.cpp
    src/mkv_writer.cpp
    src/sip_abi.cpp
)

set_target_properties(sip_core_shared PROPERTIES
    OUTPUT_NAME "sipcore"
    PREFIX "lib"
)

target_include_directories(sip_core_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${BARESIP_INCLUDE_DIR}
)

target_compile_definitions(sip_core_shared
    PRIVATE
        SIP_CORE_BUILDING_LIB=1
        SIP_CORE_BUILDING_DLL=1
)

if(WIN32 OR MINGW)
    target_link_libraries(sip_core_shared
        PRIVATE
            ws2_32
            iphlpapi
    )
elseif(APPLE)
    if(BARESIP_LIBRARY)
        target_link_libraries(sip_core_shared
            PRIVATE
                ${BARESIP_LIBRARY}
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
                "-framework Security"
                z
                m
        )
    endif()
else()
    if(BARESIP_LIBRARY)
        find_package(Threads REQUIRED)
        target_link_libraries(sip_core_shared
            PRIVATE
                ${BARESIP_LIBRARY}
                Threads::Threads
                asound
                pulse
                dl
                m
                z
        )
        target_link_options(sip_core_shared
            PRIVATE
                "-Wl,--exclude-libs,ALL"
                "-Wl,-Bsymbolic"
        )
    endif()
endif()

# Test runners
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

    add_executable(sip_core_registration_test
        tests/test_registration.cpp
    )

    target_include_directories(sip_core_registration_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${BARESIP_INCLUDE_DIR}
    )

    target_link_libraries(sip_core_registration_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    add_executable(sip_core_audio_call_test
        tests/test_audio_call.cpp
    )

    target_include_directories(sip_core_audio_call_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(sip_core_audio_call_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    add_executable(sip_core_video_streaming_test
        tests/test_video_streaming.cpp
    )

    target_include_directories(sip_core_video_streaming_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${BARESIP_INCLUDE_DIR}
    )

    target_link_libraries(sip_core_video_streaming_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    add_executable(sip_core_display_frames_test
        tests/test_display_frames.cpp
    )

    target_include_directories(sip_core_display_frames_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${BARESIP_INCLUDE_DIR}
    )

    target_link_libraries(sip_core_display_frames_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    add_executable(sip_core_conference_mixer_test
        tests/test_conference_mixer.cpp
        src/conference_mixer.cpp
    )

    target_include_directories(sip_core_conference_mixer_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    add_executable(sip_core_call_recorder_test
        tests/test_call_recorder.cpp
        src/call_recorder.cpp
        src/mkv_writer.cpp
    )

    target_include_directories(sip_core_call_recorder_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    add_executable(sip_core_multi_account_test
        tests/test_multi_account.cpp
    )

    target_include_directories(sip_core_multi_account_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(sip_core_multi_account_test
        PRIVATE
            sip_core
            ${BARESIP_LIBRARY}
    )

    if(APPLE)
        target_link_libraries(sip_core_display_frames_test
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
                "-framework Security"
                z
                m
        )

        target_link_libraries(sip_core_registration_test
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
                "-framework Security"
                z
                m
        )

        target_link_libraries(sip_core_audio_call_test
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
                "-framework Security"
                z
                m
        )

        target_link_libraries(sip_core_video_streaming_test
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
                "-framework Security"
                z
                m
        )

        target_link_libraries(sip_core_multi_account_test
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
                "-framework Security"
                z
                m
        )
    else()
        find_package(Threads REQUIRED)
        set(CORE_TEST_LINUX_LIBS
            Threads::Threads
            asound
            pulse
            dl
            m
            z
        )

        target_link_libraries(sip_core_lifecycle_test PRIVATE ${CORE_TEST_LINUX_LIBS})
        target_link_libraries(sip_core_registration_test PRIVATE ${CORE_TEST_LINUX_LIBS})
        target_link_libraries(sip_core_audio_call_test PRIVATE ${CORE_TEST_LINUX_LIBS})
        target_link_libraries(sip_core_video_streaming_test PRIVATE ${CORE_TEST_LINUX_LIBS})
        target_link_libraries(sip_core_display_frames_test PRIVATE ${CORE_TEST_LINUX_LIBS})
        target_link_libraries(sip_core_multi_account_test PRIVATE ${CORE_TEST_LINUX_LIBS})
    endif()
endif()

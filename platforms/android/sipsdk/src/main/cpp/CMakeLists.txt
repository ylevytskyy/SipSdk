cmake_minimum_required(VERSION 3.22.1)
project(sipcore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_definitions(USE_UNIXSOCK=ON)
add_compile_definitions(SIP_CORE_BUILDING_LIB=1)

# Paths to core and prebuilts
get_filename_component(SIPSDK_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(CORE_DIR "${SIPSDK_ROOT_DIR}/core")
get_filename_component(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../prebuilt" ABSOLUTE)

# Prebuilt static libraries per ABI
add_library(baresip STATIC IMPORTED)
set_target_properties(baresip PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_DIR}/${ANDROID_ABI}/lib/libbaresip.a"
)

add_library(re STATIC IMPORTED)
set_target_properties(re PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_DIR}/${ANDROID_ABI}/lib/libre.a"
)

# Build shared library libsipcore.so
add_library(sipcore SHARED
    sip_jni.cpp
    media_codec_driver.cpp
    ${CORE_DIR}/src/call_session.cpp
    ${CORE_DIR}/src/engine.cpp
    ${CORE_DIR}/src/media_driver.cpp
    ${CORE_DIR}/src/sip_abi.cpp
)

target_include_directories(sipcore
    PRIVATE
        ${CORE_DIR}/src
        ${PREBUILT_DIR}/include/re
        ${PREBUILT_DIR}/include/baresip
    PUBLIC
        ${CORE_DIR}/include
)

find_library(OPENSLES_LIB OpenSLES)
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(MEDIANDK_LIB mediandk)

target_link_libraries(sipcore
    PRIVATE
        baresip
        re
        ${OPENSLES_LIB}
        ${LOG_LIB}
        ${ANDROID_LIB}
        ${MEDIANDK_LIB}
        z
)

target_link_options(sipcore
    PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/sipcore.map"
        "-Wl,--exclude-libs,ALL"
)

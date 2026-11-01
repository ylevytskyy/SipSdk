cmake_minimum_required(VERSION 3.15)
project(desktop_sample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(desktop_sample
    main.cpp
)

target_link_libraries(desktop_sample
    PRIVATE
        sip::desktop
)

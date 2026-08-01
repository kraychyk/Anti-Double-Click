cmake_minimum_required(VERSION 3.15)
project(anti-double-click LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(anti-double-click WIN32 main.cpp app.rc)

target_compile_definitions(anti-double-click PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

target_link_libraries(anti-double-click PRIVATE shell32 user32 advapi32)

set_target_properties(anti-double-click PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    LINK_FLAGS "/SUBSYSTEM:WINDOWS"
)

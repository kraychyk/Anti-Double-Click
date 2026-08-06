cmake_minimum_required(VERSION 3.15)
project(anti-double-click LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(anti-double-click WIN32 main.cpp app.rc)

target_compile_definitions(anti-double-click PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

# /utf-8: исходник содержит кириллицу в строковых литералах.
# /MANIFEST:NO: манифест встраивается ресурсом из app.rc, два манифеста линкер не примет.
target_compile_options(anti-double-click PRIVATE /utf-8 /W4)
target_link_options(anti-double-click PRIVATE /MANIFEST:NO)

target_link_libraries(anti-double-click PRIVATE shell32 user32 advapi32)

set_target_properties(anti-double-click PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

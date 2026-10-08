cmake_minimum_required(VERSION 3.20)
project(VirtTilt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: single self-contained exe, no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(VirtTilt WIN32
    src/main.cpp
    src/app.manifest
)

target_compile_definitions(VirtTilt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(VirtTilt PRIVATE /W4 /permissive- /utf-8
        $<$<CONFIG:Release>:/O1 /GL /Gy>)
    target_link_options(VirtTilt PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()

target_link_libraries(VirtTilt PRIVATE user32 shell32 advapi32 gdi32 ole32)

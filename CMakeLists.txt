cmake_minimum_required(VERSION 3.20)
project(WireView VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "WireView is a Windows program")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: one exe, nothing to install

add_library(wvcore STATIC
  src/protocol.cpp
  src/net.cpp
  src/discovery.cpp
  src/capture.cpp
  src/inject.cpp
  src/host.cpp
  src/viewer.cpp
  src/render.cpp
  src/cursor.cpp
)
target_include_directories(wvcore PUBLIC src)
target_compile_definitions(wvcore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(wvcore PUBLIC ws2_32 iphlpapi d3d11 dxgi d3dcompiler user32 gdi32)
if(MSVC)
  target_compile_options(wvcore PUBLIC /W4 /utf-8 /permissive- /wd4100)
endif()

add_executable(WireView WIN32 src/main.cpp src/wireview.rc)
target_link_libraries(WireView PRIVATE wvcore comctl32 shell32 advapi32)
if(MSVC)
  target_link_options(WireView PRIVATE /MANIFEST:NO)  # manifest comes from wireview.rc
endif()

add_executable(wireview_tests tests/tests.cpp)
target_link_libraries(wireview_tests PRIVATE wvcore)

enable_testing()
add_test(NAME wireview_tests COMMAND wireview_tests)

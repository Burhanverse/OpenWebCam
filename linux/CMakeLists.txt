cmake_minimum_required(VERSION 3.16)
project(OpenWebCamLinux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

add_executable(openwebcam openwebcam.cpp)
target_compile_options(openwebcam PRIVATE -Wall -Wextra)
target_link_libraries(openwebcam PRIVATE PkgConfig::FFMPEG Threads::Threads)

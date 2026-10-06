cmake_minimum_required(VERSION 3.21)
project(OpenWebCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no VC++ redistributable needed on the user's PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /permissive-)

# Dev/test tool: receive + decode the phone stream, dump a frame.
add_executable(owc-probe probe/owc_probe.cpp)
target_link_libraries(owc-probe PRIVATE mfplat mfuuid wmcodecdspuuid ws2_32 iphlpapi ole32)

# Virtual camera media source, loaded by the Windows Camera Frame Server.
add_library(OpenWebCam SHARED vcam/OpenWebCam.cpp vcam/OpenWebCam.def)
target_link_libraries(OpenWebCam PRIVATE mfplat mfuuid wmcodecdspuuid ws2_32 iphlpapi ole32 advapi32 runtimeobject)

# Installer/uninstaller (requires admin via manifest).
add_executable(OpenWebCamSetup setup/setup.cpp setup/setup.manifest)
target_link_libraries(OpenWebCamSetup PRIVATE mfplat mfsensorgroup ole32 advapi32 iphlpapi ws2_32)
target_link_options(OpenWebCamSetup PRIVATE /MANIFESTUAC:NO)

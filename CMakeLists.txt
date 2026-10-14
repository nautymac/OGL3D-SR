cmake_minimum_required(VERSION 3.20)
# OGL3D: opengl32.dll proxy that renders OpenGL games in stereo 3D (SR weaving or half SBS).
# Builds for x64 (-A x64) and x86 (-A Win32); see build.ps1.
project(OGL3D VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL lands in arbitrary game folders.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(SR_SDK_ROOT "D:/OneDrive/3D/LeiaSR Runtime Installer(SDK)")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OGL3D_ARCH x64)
    enable_language(ASM_MASM)
    set(OGL3D_FORWARD src/gen/forward_x64.asm)
    set(SR_SDK_DEFAULT "${SR_SDK_ROOT}/simulatedreality-1.34.10-win64-Release (extracted SDK)")
    set(SR_SUFFIX "")
else()
    set(OGL3D_ARCH x86)
    set(OGL3D_FORWARD src/gen/forward_x86.cpp)
    set(SR_SDK_DEFAULT "${SR_SDK_ROOT}/simulatedreality-1.34.10-win32-Release (extracted SDK)")
    set(SR_SUFFIX "32")   # SimulatedRealityCore32.dll etc.
endif()
set(OGL3D_OUT_DIR "${CMAKE_SOURCE_DIR}/bin/${OGL3D_ARCH}" CACHE PATH "output folder")

set(SR_SDK "${SR_SDK_DEFAULT}" CACHE PATH "extracted simulatedreality-<ver>-win64/win32-Release SDK for this architecture")
if(NOT EXISTS "${SR_SDK}/include/sr/weaver/glweaver.h")
    message(FATAL_ERROR "SR SDK not found at ${SR_SDK} (-DSR_SDK=<path>)")
endif()


# version resource: the installer recognises the DLL by its product name "OGL3D"
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

add_library(OGL3D SHARED
    src/proxy.cpp
    src/stereo.cpp
    src/inject.cpp
    src/srout.cpp
    src/osd.cpp
    src/present.cpp
    ${OGL3D_FORWARD}
    src/gen/opengl32_${OGL3D_ARCH}.def
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
target_include_directories(OGL3D PRIVATE
    "${SR_SDK}/include"
    "${SR_SDK}/third_party/OpenCV/include"
    "${SR_SDK}/third_party/GLog/include")
# /wd4005: the SDK's IWeaverBase.h redefines WIN32_LEAN_AND_MEAN
target_compile_options(OGL3D PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc /permissive- /utf-8 /W3 /wd4005>)
target_compile_definitions(OGL3D PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    OGL3D_VERSION="${PROJECT_VERSION}")
# No opengl32 import lib: this DLL *is* opengl32.dll; the real one is loaded at runtime.
# The SR runtime is delay-loaded from the SR Platform folder (srout.cpp), so SR is optional.
target_link_libraries(OGL3D PRIVATE gdi32 user32 delayimp
    "${SR_SDK}/lib/SimulatedRealityCore${SR_SUFFIX}.lib"
    "${SR_SDK}/lib/SimulatedRealityOpenGL${SR_SUFFIX}.lib"
    "${SR_SDK}/lib/SimulatedRealityDirectX${SR_SUFFIX}.lib"
    "${SR_SDK}/lib/SimulatedRealityDisplays${SR_SUFFIX}.lib"
    "${SR_SDK}/third_party/OpenCV/lib/${OGL3D_ARCH}/opencv_world343.lib")
target_link_options(OGL3D PRIVATE
    /MAP
    /DELAYLOAD:SimulatedRealityCore${SR_SUFFIX}.dll
    /DELAYLOAD:SimulatedRealityOpenGL${SR_SUFFIX}.dll
    /DELAYLOAD:SimulatedRealityDirectX${SR_SUFFIX}.dll
    /DELAYLOAD:SimulatedRealityDisplays${SR_SUFFIX}.dll
    /DELAYLOAD:opencv_world343.dll)
set_target_properties(OGL3D PROPERTIES
    OUTPUT_NAME opengl32
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OGL3D_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OGL3D_OUT_DIR}")
if(OGL3D_ARCH STREQUAL x86)
    # the SDK's 32-bit import libraries are not SAFESEH-registered; naked stubs need no SEH either
    target_link_options(OGL3D PRIVATE /SAFESEH:NO)
endif()

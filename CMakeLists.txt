cmake_minimum_required(VERSION 3.25)
project(FNVVRTestBridge VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Fallout: New Vegas is 32-bit: configure with the x86 preset (-A Win32).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime: the plugin runs on PCs without Visual Studio.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(XNVSE ${CMAKE_SOURCE_DIR}/external/xNVSE)
set(BRIDGE_DEFS BRIDGE_VERSION="${PROJECT_VERSION}"
    BRIDGE_VERSION_NUMBER=${PROJECT_VERSION_MAJOR}${PROJECT_VERSION_MINOR}${PROJECT_VERSION_PATCH}
    _CRT_SECURE_NO_WARNINGS)

# Game-free parts (protocol), shared by the plugin and the tests
add_library(bridge_core STATIC src/protocol.cpp)
target_include_directories(bridge_core PUBLIC src)
target_compile_definitions(bridge_core PUBLIC ${BRIDGE_DEFS})
target_compile_options(bridge_core PRIVATE /W4)
target_compile_options(bridge_core PUBLIC /utf-8)

# The plugin the game loads (xNVSE headers only; no xNVSE sources are compiled)
add_library(FNVVR-Test-Bridge SHARED
    src/main.cpp
    src/game.cpp
    src/pipe_server.cpp
    src/log.cpp
    src/memory_status.cpp
    src/exports.def
)
target_include_directories(FNVVR-Test-Bridge PRIVATE ${XNVSE} ${XNVSE}/nvse ${XNVSE}/nvse/nvse)
target_compile_definitions(FNVVR-Test-Bridge PRIVATE RUNTIME RUNTIME_VERSION=0x040020D0 _WINDOWS _USRDLL)
target_compile_options(FNVVR-Test-Bridge PRIVATE /FInvse/prefix.h /W3 /MP)
target_link_libraries(FNVVR-Test-Bridge PRIVATE bridge_core advapi32)

enable_testing()
add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE bridge_core)
add_test(NAME protocol COMMAND protocol_test)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME python_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_fnvtest.py)
endif()

# Headset-free OpenXR runtime for automated tests of FNVVR's OpenXR backend (openxr-null-runtime/README.md).
# Khronos headers: external/openxr (Apache-2.0 OR MIT), external/vulkan (Apache-2.0 OR MIT).
add_library(fnvvr_null_xr SHARED
    openxr-null-runtime/fnvvr_null_xr.cpp
    openxr-null-runtime/fnvvr_null_xr.def
)
target_include_directories(fnvvr_null_xr PRIVATE ${CMAKE_SOURCE_DIR}/external/openxr/include ${CMAKE_SOURCE_DIR}/external/vulkan/include)
target_compile_definitions(fnvvr_null_xr PRIVATE _CRT_SECURE_NO_WARNINGS XR_NO_PROTOTYPES VK_NO_PROTOTYPES)
target_compile_options(fnvvr_null_xr PRIVATE /W4 /utf-8)
add_custom_command(TARGET fnvvr_null_xr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/openxr-null-runtime/fnvvr_null_xr.json $<TARGET_FILE_DIR:fnvvr_null_xr>
)
add_executable(null_runtime_test tests/null_runtime_test.cpp)
target_include_directories(null_runtime_test PRIVATE ${CMAKE_SOURCE_DIR}/external/openxr/include)
add_dependencies(null_runtime_test fnvvr_null_xr)
add_test(NAME null_runtime COMMAND null_runtime_test $<TARGET_FILE:fnvvr_null_xr>)
add_executable(loader_probe tests/loader_probe.cpp)
target_include_directories(loader_probe PRIVATE ${CMAKE_SOURCE_DIR}/external/openxr/include)

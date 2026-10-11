cmake_minimum_required(VERSION 3.24)
project(MyWallpaperTaskbarConfigurator VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Taskbar Configurator requires Windows x64 and MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_TESTING OFF CACHE BOOL "Build the engine's fixture suite" FORCE)

get_filename_component(_sibling_engine "${CMAKE_CURRENT_SOURCE_DIR}/../mywallpaper-native-engine" ABSOLUTE)
set(MW_NATIVE_ENGINE_SOURCE_DIR "${_sibling_engine}" CACHE PATH
    "Pinned sibling checkout of mywallpaper-native-engine")
if(NOT EXISTS "${MW_NATIVE_ENGINE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set MW_NATIVE_ENGINE_SOURCE_DIR to the native engine checkout")
endif()

add_subdirectory("${MW_NATIVE_ENGINE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/native-engine")

add_library(taskbar_configurator SHARED
    src/config.cpp
    src/taskbar_configurator.cpp
    src/taskbar_configurator.def)
target_link_libraries(taskbar_configurator PRIVATE
    MyWallpaperNativeEngine::SDK
    windowsapp
    ole32
    oleaut32
    user32)
target_include_directories(taskbar_configurator PRIVATE include)
target_compile_options(taskbar_configurator PRIVATE
    /utf-8 /W4 /permissive- /EHsc /Zc:preprocessor /Brepro /experimental:deterministic
    "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=.")
target_link_options(taskbar_configurator PRIVATE /Brepro)
set_target_properties(taskbar_configurator PROPERTIES
    OUTPUT_NAME "addon"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

add_executable(taskbar_config_tests
    src/config.cpp
    tests/config_tests.cpp)
target_link_libraries(taskbar_config_tests PRIVATE MyWallpaperNativeEngine::SDK)
target_include_directories(taskbar_config_tests PRIVATE include)
target_compile_options(taskbar_config_tests PRIVATE
    /utf-8 /W4 /permissive- /EHsc /Zc:preprocessor /Brepro /experimental:deterministic
    "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=.")

enable_testing()
add_test(NAME taskbar_config_contract COMMAND taskbar_config_tests)

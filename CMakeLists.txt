cmake_minimum_required(VERSION 3.24)

project(WaibuSnap VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
elseif(NOT WIN32)
    message(FATAL_ERROR "当前工程仅支持 macOS 和 Windows。")
endif()

find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Widgets)
foreach(component IN ITEMS Core Gui Widgets)
    get_target_property(qt_library_type Qt6::${component} TYPE)
    if(NOT qt_library_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "Qt ${component} 必须使用动态库，当前为 ${qt_library_type}。")
    endif()
endforeach()

qt_standard_project_setup()
include(CTest)

add_library(waibusnap_options INTERFACE)
if(MSVC)
    target_compile_options(waibusnap_options INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(waibusnap_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(src)
if(BUILD_TESTING)
    find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# 构建时元数据与实际测量时元数据分别保存，防止测量工具误标构建 commit。
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE WAIBUSNAP_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE WAIBUSNAP_BUILD_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE)
if(WAIBUSNAP_BUILD_DIRTY)
    set(WAIBUSNAP_BUILD_DIRTY_JSON true)
else()
    set(WAIBUSNAP_BUILD_DIRTY_JSON false)
endif()
if(APPLE)
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE WAIBUSNAP_BUILD_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND xcrun --show-sdk-version OUTPUT_VARIABLE WAIBUSNAP_BUILD_SDK_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/build-metadata-$<CONFIG>.json" CONTENT
"{\"commit\":\"${WAIBUSNAP_BUILD_COMMIT}\",\"dirty\":${WAIBUSNAP_BUILD_DIRTY_JSON},\"configuration\":\"$<CONFIG>\",\"compiler_path\":\"${CMAKE_CXX_COMPILER}\",\"compiler_id\":\"${CMAKE_CXX_COMPILER_ID}\",\"compiler_version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"qt_version\":\"${Qt6_VERSION}\",\"sdk\":\"${WAIBUSNAP_BUILD_SDK}\",\"sdk_version\":\"${WAIBUSNAP_BUILD_SDK_VERSION}\",\"deployment_target\":\"${CMAKE_OSX_DEPLOYMENT_TARGET}\",\"architecture\":\"${CMAKE_OSX_ARCHITECTURES}\"}\n")

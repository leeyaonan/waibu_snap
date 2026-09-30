add_library(waibusnap_platform STATIC)
target_include_directories(waibusnap_platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(waibusnap_platform PUBLIC Qt6::Core PRIVATE waibusnap_options)

if(APPLE)
    target_sources(waibusnap_platform PRIVATE macos/platform_info.mm)
elseif(WIN32)
    target_sources(waibusnap_platform PRIVATE windows/platform_info.cpp)
endif()

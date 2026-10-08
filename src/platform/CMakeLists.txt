add_library(waibusnap_platform STATIC)
target_include_directories(waibusnap_platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(waibusnap_platform PUBLIC Qt6::Gui PRIVATE waibusnap_options)
set(waibusnap_tray_assets "${PROJECT_SOURCE_DIR}/assets/icons/tray")
qt_add_resources(waibusnap_platform waibusnap_tray_icons
    PREFIX "/icons/tray"
    BASE "${waibusnap_tray_assets}"
    FILES
        "${waibusnap_tray_assets}/waibusnap-tray-mask.png"
        "${waibusnap_tray_assets}/waibusnap-tray-mask@2x.png"
        "${waibusnap_tray_assets}/waibusnap-tray-color.png"
        "${waibusnap_tray_assets}/waibusnap-tray-color@2x.png"
)
if(APPLE)
    target_sources(waibusnap_platform PRIVATE macos/platform_info.mm macos/capture_services.mm
        macos/presentation_observer.mm macos/platform_workarounds.mm macos/sticker_window_behavior.mm
        macos/tray_icon.mm)
    target_compile_options(waibusnap_platform PRIVATE -fobjc-arc)
    foreach(framework IN ITEMS AppKit Carbon ScreenCaptureKit CoreGraphics QuartzCore)
        find_library(${framework}_FRAMEWORK ${framework} REQUIRED)
        target_link_libraries(waibusnap_platform PRIVATE "${${framework}_FRAMEWORK}")
    endforeach()
elseif(WIN32)
    target_sources(waibusnap_platform PRIVATE windows/platform_info.cpp windows/capture_services.cpp
        windows/platform_workarounds.cpp windows/sticker_window_behavior.cpp windows/tray_icon.cpp)
endif()

if(APPLE)
    add_executable(waibusnap_measure_probe macos/measure_process.mm)
    target_link_libraries(waibusnap_measure_probe PRIVATE Qt6::Core waibusnap_options
        "${AppKit_FRAMEWORK}" "${Carbon_FRAMEWORK}" "${CoreGraphics_FRAMEWORK}")
    target_compile_options(waibusnap_measure_probe PRIVATE -fobjc-arc)
    set_target_properties(waibusnap_measure_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

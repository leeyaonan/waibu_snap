add_subdirectory(platform)

add_library(waibusnap_core STATIC core/annotation.cpp core/selection_geometry.cpp core/selection_assistance.cpp core/window_snapping.cpp core/sticker_geometry.cpp
    session/session_metrics.cpp)
target_include_directories(waibusnap_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(waibusnap_core PUBLIC Qt6::Gui PRIVATE waibusnap_options)
add_library(waibusnap_output STATIC output/image_output.cpp output/annotation_renderer.cpp)
target_include_directories(waibusnap_output PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(waibusnap_output PUBLIC Qt6::Gui waibusnap_core PRIVATE waibusnap_options)
add_library(waibusnap_settings STATIC app/hotkey_rules.cpp app/app_settings.cpp)
target_include_directories(waibusnap_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(waibusnap_settings PUBLIC Qt6::Gui PRIVATE waibusnap_options)
add_library(waibusnap_ui STATIC output/image_save.cpp ui/selection_overlay.cpp ui/selection_overlay.h
    ui/annotation_text_edit.cpp ui/annotation_text_edit.h
    ui/settings_dialog.cpp ui/settings_dialog.h
    app/application_controller.cpp app/application_controller.h
    app/sticker_manager.cpp app/sticker_manager.h ui/sticker_window.cpp ui/sticker_window.h)
target_include_directories(waibusnap_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(waibusnap_ui PUBLIC Qt6::Widgets waibusnap_core waibusnap_output waibusnap_settings waibusnap_platform PRIVATE waibusnap_options)

qt_add_executable(WaibuSnap app/main.cpp)
target_link_libraries(WaibuSnap PRIVATE waibusnap_ui waibusnap_platform waibusnap_options)
set_target_properties(WaibuSnap PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(APPLE)
    set(waibusnap_icon "${PROJECT_SOURCE_DIR}/assets/icons/waibusnap.icns")
    set_source_files_properties("${waibusnap_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(WaibuSnap PRIVATE "${waibusnap_icon}")
    # 仅为开发占位，可在配置时覆盖；正式 bundle id 仍待用户确认。
    set(WAIBUSNAP_BUNDLE_ID "local.waibusnap.dev" CACHE STRING "开发用 bundle id")
    set_target_properties(WaibuSnap PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${WAIBUSNAP_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "WaibuSnap"
        MACOSX_BUNDLE_ICON_FILE "waibusnap.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

if(WIN32)
    enable_language(RC)
    target_sources(WaibuSnap PRIVATE "${PROJECT_SOURCE_DIR}/assets/icons/waibusnap.rc")
    target_include_directories(WaibuSnap PRIVATE "${PROJECT_SOURCE_DIR}/assets/icons")
endif()

if(BUILD_TESTING)
    # 只在测试构建中提供自动关闭入口；发布构建关闭 BUILD_TESTING。
    target_compile_definitions(WaibuSnap PRIVATE WAIBUSNAP_ENABLE_SMOKE_TEST)
endif()

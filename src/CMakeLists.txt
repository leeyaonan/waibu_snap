add_subdirectory(platform)

add_library(waibusnap_ui STATIC ui/startup_window.cpp ui/startup_window.h)
target_include_directories(waibusnap_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(waibusnap_ui PUBLIC Qt6::Widgets PRIVATE waibusnap_options)

qt_add_executable(WaibuSnap app/main.cpp)
target_link_libraries(WaibuSnap PRIVATE waibusnap_ui waibusnap_platform waibusnap_options)
set_target_properties(WaibuSnap PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(APPLE)
    # 仅为开发占位，可在配置时覆盖；正式 bundle id 仍待用户确认。
    set(WAIBUSNAP_BUNDLE_ID "local.waibusnap.dev" CACHE STRING "开发用 bundle id")
    set_target_properties(WaibuSnap PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "${WAIBUSNAP_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "WaibuSnap"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

if(BUILD_TESTING)
    # 只在测试构建中提供自动关闭入口；发布构建关闭 BUILD_TESTING。
    target_compile_definitions(WaibuSnap PRIVATE WAIBUSNAP_ENABLE_SMOKE_TEST)
endif()

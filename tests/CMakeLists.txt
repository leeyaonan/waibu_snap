qt_add_executable(startup_smoke integration/startup_smoke.cpp)
target_link_libraries(startup_smoke PRIVATE waibusnap_ui Qt6::Test waibusnap_options)
if(APPLE)
    target_compile_definitions(startup_smoke PRIVATE
        WAIBUSNAP_TEST_TRAY_SIZE=18 WAIBUSNAP_TEST_TRAY_MASK=1)
elseif(WIN32)
    target_compile_definitions(startup_smoke PRIVATE
        WAIBUSNAP_TEST_TRAY_SIZE=16 WAIBUSNAP_TEST_TRAY_MASK=0)
endif()
add_dependencies(startup_smoke WaibuSnap)
# 测试保持控制台子系统：Windows 上 GUI 子系统的 Qt Test 输出不会进入 ctest 捕获。
set_target_properties(startup_smoke PROPERTIES WIN32_EXECUTABLE FALSE)

# 同时把 Qt Test 日志写入文件，避免子系统差异影响失败诊断。
add_test(NAME startup_smoke COMMAND startup_smoke -o
    "${CMAKE_CURRENT_BINARY_DIR}/startup_smoke-output.txt,txt")
set_tests_properties(startup_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAIBUSNAP_TEST_APP=$<TARGET_FILE:WaibuSnap>"
    TIMEOUT 30
)

qt_add_executable(selection_geometry_test unit/selection_geometry_test.cpp)
target_link_libraries(selection_geometry_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(selection_geometry_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME selection_geometry COMMAND selection_geometry_test)

qt_add_executable(image_output_test unit/image_output_test.cpp)
target_link_libraries(image_output_test PRIVATE waibusnap_output Qt6::Test waibusnap_options)
set_target_properties(image_output_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME image_output COMMAND image_output_test)
set_tests_properties(image_output PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(annotation_test unit/annotation_test.cpp)
target_link_libraries(annotation_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(annotation_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME annotation COMMAND annotation_test)

qt_add_executable(window_snapping_test unit/window_snapping_test.cpp)
target_link_libraries(window_snapping_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(window_snapping_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME window_snapping COMMAND window_snapping_test)

qt_add_executable(hotkey_settings_test unit/hotkey_settings_test.cpp)
target_link_libraries(hotkey_settings_test PRIVATE waibusnap_settings Qt6::Widgets Qt6::Test waibusnap_options)
set_target_properties(hotkey_settings_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME hotkey_settings COMMAND hotkey_settings_test)
set_tests_properties(hotkey_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME measurement_tool COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/measurement_tool_test.py")

if(APPLE)
    qt_add_executable(platform_workarounds_test unit/platform_workarounds_test.mm)
    target_link_libraries(platform_workarounds_test PRIVATE waibusnap_platform Qt6::Test
        waibusnap_options "${AppKit_FRAMEWORK}")
    target_compile_options(platform_workarounds_test PRIVATE -fobjc-arc)
    add_test(NAME platform_workarounds COMMAND platform_workarounds_test)
    set_tests_properties(platform_workarounds PROPERTIES TIMEOUT 10)

    add_test(NAME probe_cpu_calibration COMMAND waibusnap_measure_probe --self-test)
    set_tests_properties(probe_cpu_calibration PROPERTIES TIMEOUT 10)
endif()

qt_add_executable(sticker_geometry_test unit/sticker_geometry_test.cpp)
target_link_libraries(sticker_geometry_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(sticker_geometry_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME sticker_geometry COMMAND sticker_geometry_test)

add_test(NAME startup_smoke_sticker_high_dpi COMMAND startup_smoke
    trayIconUsesEmbeddedPlatformAssets
    stickerWindowsScaleDragAndCloseIndependently stickerOutputsKeepOriginalPixelsAndRetryFailures
    stickerEditingEntrancesGesturesAndCompactTools stickerTextImeEscapeAndCommitBoundaries
    stickerHistoryStylesOutputAndDirtyState stickerPendingTextCommitsBeforeOutputsAndNewDrawing
    stickerCloseSaveFailureRestoresEditingToolbar
    stickerHideRestorePreservesStateAndHistory stickerHideCommitsPendingTextWithoutConfirmation
    stickerHiddenCloseAndCleanupDoNotResurrect stickerHiddenQuitDecisionsAndReentrancy
    stickerHiddenQuitDefaultDialogStillConfirms stickerHiddenRecoveryPreservesVisibility
    stickerMenuAvailabilityAndOverlayIsolation
    coverOverlayGesturesHistoryAndOutput coverStickerGesturesHistoryAndOutput
    mosaicOverlayGesturesHistoryAndOutput mosaicStickerGesturesHistoryAndOutput
    selectionNudgeKeys selectionNudgeLimits selectionNudgeIgnoresModifiersAndControlFocus
    selectionNudgeToolAndTextIsolation selectionNudgeHistoryDirtyAndOutput
    selectionNudgeSavePanelAndFinishedIsolation magnifierMouseSampling
    magnifierFollowsSelectionGestures magnifierPaintUsesNearestPixels magnifierKeyboardTimeoutAndCleanup
    quickSaveDirectoryCollisionAndFormat quitSaveAllUsesQuickDirectoryWithoutCollisions
    savePanelFormatsNormalizeSuffixAndDispatch
    clipboardStickerImageLifecycle clipboardStickerEmptyToastReuseTimeoutAndCleanup
    clipboardStickerFailureDoesNotAdvanceCascade clipboardStickerMenuSessionAndQuitGuards
    clipboardStickerReadOnly
    clipboardStickerToastClampsAtScreenEdges clipboardStickerCleanupDisablesAction
    autostartToggleAndReopen autostartFailureKeepsStateAndRetries
    autostartUnchangedCancelAndEscape autostartSaveQueriesCurrentSystemState
    autostartApprovalAndNotice autostartCombinedSaveOrderAndFailures
    controllerAutostartMenuSaveReopenAndCancel
    -o "${CMAKE_CURRENT_BINARY_DIR}/startup_smoke-sticker-high-dpi-output.txt,txt")
set_tests_properties(startup_smoke_sticker_high_dpi PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2"
    TIMEOUT 30
)

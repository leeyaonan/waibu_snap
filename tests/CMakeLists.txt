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
    # 紧凑工具栏增加可用区矩阵与高 DPI 子集，给完整断言留足执行时间。
    TIMEOUT 60
)

qt_add_executable(annotation_text_edit_test unit/annotation_text_edit_test.cpp)
target_link_libraries(annotation_text_edit_test PRIVATE waibusnap_ui Qt6::Test waibusnap_options)
set_target_properties(annotation_text_edit_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME annotation_text_edit COMMAND annotation_text_edit_test)
set_tests_properties(annotation_text_edit PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(selection_geometry_test unit/selection_geometry_test.cpp)
target_link_libraries(selection_geometry_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(selection_geometry_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME selection_geometry COMMAND selection_geometry_test)

qt_add_executable(image_output_test unit/image_output_test.cpp)
target_link_libraries(image_output_test PRIVATE waibusnap_output Qt6::Test waibusnap_options)
set_target_properties(image_output_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME image_output COMMAND image_output_test -o
    "${CMAKE_CURRENT_BINARY_DIR}/image_output-output.txt,txt")
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

qt_add_executable(history_types_test unit/history_types_test.cpp)
target_link_libraries(history_types_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(history_types_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME history_types COMMAND history_types_test)

qt_add_executable(history_cleanup_test unit/history_cleanup_test.cpp)
target_link_libraries(history_cleanup_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(history_cleanup_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME history_cleanup COMMAND history_cleanup_test)

qt_add_executable(history_store_test unit/history_store_test.cpp)
target_link_libraries(history_store_test PRIVATE waibusnap_settings Qt6::Test waibusnap_options)
set_target_properties(history_store_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME history_store COMMAND history_store_test)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME measurement_tool COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/measurement_tool_test.py")

if(APPLE)
    qt_add_executable(platform_workarounds_test unit/platform_workarounds_test.mm)
    target_link_libraries(platform_workarounds_test PRIVATE waibusnap_platform Qt6::Test
        waibusnap_options "${AppKit_FRAMEWORK}")
    target_compile_options(platform_workarounds_test PRIVATE -fobjc-arc)
    add_test(NAME platform_workarounds COMMAND platform_workarounds_test)
    set_tests_properties(platform_workarounds PROPERTIES TIMEOUT 30)

    add_test(NAME probe_cpu_calibration COMMAND waibusnap_measure_probe --self-test)
    set_tests_properties(probe_cpu_calibration PROPERTIES TIMEOUT 10)
endif()

qt_add_executable(sticker_geometry_test unit/sticker_geometry_test.cpp)
target_link_libraries(sticker_geometry_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(sticker_geometry_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME sticker_geometry COMMAND sticker_geometry_test)

add_test(NAME startup_smoke_sticker_high_dpi COMMAND startup_smoke
    scrollReadyDprAndClamping scrollEntryPreparation scrollReadyTransitions scrollPlatformAndMenu scrollControlPlacement
    scrollAutomaticPermissionAndSuspension scrollAutomaticProbeAndBottom scrollManualBottomAndLimit
    scrollResultPreviewCropOutput scrollResultFullOverview scrollResultCloseConfirmation scrollControllerLifecycle
    scrollControllerRetryAndQuit scrollControllerPermissionDialogAndQuitCancel
    scrollResultSavePinAndLimit scrollUnreliableFramesNeverTriggerBottom
    scrollLatestUnreliableFramePreventsBottom scrollHiccupsHaveNoWarningOrRetry scrollCropRetainsCaptureAxis
    scrollBudgetExhaustionIsVisible
    fullScreenToolbarUsesInsideBottom toolbarStaysReachableWithSystemChrome
    toolbarCompactIconsAndTooltips toolbarOptionsFollowActiveTool toolbarStatusPillIndependent
    toolbarStatusPillTimesOut
    annotationPaletteReachableAndOutput annotationWheelBothEditors
    annotationTextTransparentBothEditors
    annotationGroupDefaultsAndMemory annotationGroupPanelGesturesAndPlacement stickerAnnotationGroupsAndMenu
    coverOverlayToolbarFitsNarrowScreens annotationStylesUndoRedoAndDirtyState
    toolbarBackgroundCommitsAndClearsPreedit
    shapeToolbarButtonAndBar shapeRatioExclusion shapeShiftRatioLock shapeOutputPixels
    shapeTemplateRoundTrip shapeStickerTransparency shapeBarLayoutAndPreview shapeTemplateOutputAndOcr
    ratioToolbarButtonAndBar ratioConstrainsDragAndResize ratioQuickRatioWithShift
    ratioSelectionImmediateAdjust ratioSizeTextFormats ratioKeyboardAndWindowSnap
    ratioBarLayoutAndPanelGestures ratioAllDisabledOnlyFree ratioSettingsDialogRoundTrip
    ratioSettingsCancelEmptyAndSaveFailure
    settingsTabsReachability settingsTabsDraftAndPersistence
    trayIconUsesEmbeddedPlatformAssets
    ocrModeLifecycle ocrSelection ocrCopyDispatchAndSavedState
    ocrDelayedProgressExitAndStaleResult ocrDialogEscAndRetry ocrDialogLoadingAndEmpty
    ocrDialogFormats ocrPreviewLocalCopyIsPlain ocrInputUsesAnnotatedPixelsAndFallback
    ocrTaskDestructionWaits
    stickerWindowsScaleDragAndCloseIndependently stickerOutputsKeepOriginalPixelsAndRetryFailures
    stickerEditingEntrancesGesturesAndCompactTools stickerTextImeEscapeAndCommitBoundaries
    stickerHistoryStylesOutputAndDirtyState stickerPendingTextCommitsBeforeOutputsAndNewDrawing
    stickerCloseReminderDialog stickerCloseReminderWriteFailureStillConfirms
    controllerStickerCloseReminderSettingsAndClipboard stickerCloseReminderSettingsFailureAndRetry
    stickerCloseDecisionsAndSaveRetries stickerQuitSummaryOrderFailureAndReentrancy
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
    controllerNativeInvisibleRegression controllerNativeVisibilityRecoversAndReadies
    controllerVisibilityModalAndPanelGuard controllerVisibleSessionPressure
    controllerSessionWatchdogRecovery controllerSessionWatchdogGuards
    controllerFailureHidesOverlayBeforeDialog controllerQuitDialogHidesAndRestoresOverlay
    clipboardStickerReadOnly
    clipboardStickerToastClampsAtScreenEdges clipboardStickerCleanupDisablesAction
    autostartToggleAndReopen autostartFailureKeepsStateAndRetries
    autostartUnchangedCancelAndEscape autostartSaveQueriesCurrentSystemState
    autostartApprovalAndNotice autostartCombinedSaveOrderAndFailures
    controllerAutostartMenuSaveReopenAndCancel
    -o "${CMAKE_CURRENT_BINARY_DIR}/startup_smoke-sticker-high-dpi-output.txt,txt")
set_tests_properties(startup_smoke_sticker_high_dpi PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2"
    # 紧凑工具栏增加可用区矩阵与高 DPI 子集，给完整断言留足执行时间。
    TIMEOUT 60
)

qt_add_executable(text_layout_test unit/text_layout_test.cpp)
target_link_libraries(text_layout_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(text_layout_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME text_layout COMMAND text_layout_test)

qt_add_executable(text_recognizer_test integration/text_recognizer_test.cpp)
target_link_libraries(text_recognizer_test PRIVATE waibusnap_ui Qt6::Test waibusnap_options)
target_compile_definitions(text_recognizer_test PRIVATE
    WAIBUSNAP_OCR_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ocr-mixed-text.png")
set_target_properties(text_recognizer_test PROPERTIES WIN32_EXECUTABLE FALSE)
if(APPLE)
    target_sources(text_recognizer_test PRIVATE integration/ocr_engine_languages_macos.mm)
    target_compile_options(text_recognizer_test PRIVATE -fobjc-arc)
    target_link_libraries(text_recognizer_test PRIVATE "${Vision_FRAMEWORK}")
elseif(WIN32)
    target_sources(text_recognizer_test PRIVATE integration/ocr_engine_languages_windows.cpp)
    target_link_libraries(text_recognizer_test PRIVATE windowsapp)
endif()
add_test(NAME text_recognizer_real_engine COMMAND text_recognizer_test -o
    "${CMAKE_CURRENT_BINARY_DIR}/text-recognizer-output.txt,txt")
set_tests_properties(text_recognizer_real_engine PROPERTIES TIMEOUT 120)

qt_add_executable(aspect_ratio_test unit/aspect_ratio_test.cpp)
target_link_libraries(aspect_ratio_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(aspect_ratio_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME aspect_ratio COMMAND aspect_ratio_test)

qt_add_executable(shape_mask_test unit/shape_mask_test.cpp)
target_link_libraries(shape_mask_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(shape_mask_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME shape_mask COMMAND shape_mask_test)

qt_add_executable(scroll_stitcher_test unit/scroll_stitcher_test.cpp)
target_link_libraries(scroll_stitcher_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
set_target_properties(scroll_stitcher_test PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME scroll_stitcher COMMAND scroll_stitcher_test -o
    "${CMAKE_CURRENT_BINARY_DIR}/scroll-stitcher-output.txt,txt")
set_tests_properties(scroll_stitcher PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 独立进程配合系统峰值 RSS 计量，不受整个单测进程的分配器历史影响。
qt_add_executable(scroll_memory_probe integration/scroll_memory_probe.cpp)
target_link_libraries(scroll_memory_probe PRIVATE waibusnap_core waibusnap_options)
set_target_properties(scroll_memory_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# 真实浏览器取证需人工摆放窗口及已有权限，不纳入无头 CTest。
qt_add_executable(scroll_capture_probe integration/scroll_capture_probe.cpp)
target_link_libraries(scroll_capture_probe PRIVATE waibusnap_ui waibusnap_options)
set_target_properties(scroll_capture_probe PROPERTIES WIN32_EXECUTABLE FALSE)

if(APPLE)
    target_sources(scroll_capture_probe PRIVATE integration/scroll_probe_display_macos.mm)
    target_compile_options(scroll_capture_probe PRIVATE -fobjc-arc)
    target_link_libraries(scroll_capture_probe PRIVATE "${AppKit_FRAMEWORK}")
    set_target_properties(scroll_capture_probe PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.waibusnap.scroll-probe")
elseif(WIN32)
    target_sources(scroll_capture_probe PRIVATE integration/scroll_probe_display_windows.cpp)
endif()

# 真实采集验收使用已有屏幕选择桥接，不修改生产的光标选屏路径，不加入无人值守 CTest。
qt_add_executable(overlay_visibility_probe integration/overlay_visibility_probe.cpp)
target_link_libraries(overlay_visibility_probe PRIVATE waibusnap_ui waibusnap_options)
set_target_properties(overlay_visibility_probe PROPERTIES WIN32_EXECUTABLE FALSE)
if(APPLE)
    target_sources(overlay_visibility_probe PRIVATE integration/scroll_probe_display_macos.mm)
    target_compile_options(overlay_visibility_probe PRIVATE -fobjc-arc)
    target_link_libraries(overlay_visibility_probe PRIVATE "${AppKit_FRAMEWORK}")
    set_target_properties(overlay_visibility_probe PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.waibusnap.visibility-probe")
elseif(WIN32)
    target_sources(overlay_visibility_probe PRIVATE integration/scroll_probe_display_windows.cpp)
endif()

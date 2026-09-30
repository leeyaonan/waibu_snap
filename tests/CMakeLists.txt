qt_add_executable(startup_smoke integration/startup_smoke.cpp)
target_link_libraries(startup_smoke PRIVATE waibusnap_ui Qt6::Test waibusnap_options)
add_dependencies(startup_smoke WaibuSnap)

add_test(NAME startup_smoke COMMAND startup_smoke)
set_tests_properties(startup_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAIBUSNAP_TEST_APP=$<TARGET_FILE:WaibuSnap>"
    TIMEOUT 30
)

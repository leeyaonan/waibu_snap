qt_add_executable(startup_smoke integration/startup_smoke.cpp)
target_link_libraries(startup_smoke PRIVATE waibusnap_ui Qt6::Test waibusnap_options)
add_dependencies(startup_smoke WaibuSnap)

add_test(NAME startup_smoke COMMAND startup_smoke)
set_tests_properties(startup_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAIBUSNAP_TEST_APP=$<TARGET_FILE:WaibuSnap>"
    TIMEOUT 30
)

qt_add_executable(selection_geometry_test unit/selection_geometry_test.cpp)
target_link_libraries(selection_geometry_test PRIVATE waibusnap_core Qt6::Test waibusnap_options)
add_test(NAME selection_geometry COMMAND selection_geometry_test)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME measurement_tool COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/measurement_tool_test.py")

if(APPLE)
    add_test(NAME probe_cpu_calibration COMMAND waibusnap_measure_probe --self-test)
    set_tests_properties(probe_cpu_calibration PROPERTIES TIMEOUT 10)
endif()

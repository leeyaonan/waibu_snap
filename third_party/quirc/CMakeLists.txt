add_library(waibusnap_quirc STATIC quirc.c decode.c identify.c version_db.c)
target_include_directories(waibusnap_quirc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(waibusnap_quirc PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    # 第三方 C 代码不参与本仓库的 /W4 门禁，避免无意义的上游告警淹没自有代码。
    target_compile_options(waibusnap_quirc PRIVATE /W0)
endif()

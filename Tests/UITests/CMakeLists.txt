# UI 모듈 단위 테스트 (레이아웃/에셋/입력/글꼴 — GPU 불필요)
file(GLOB_RECURSE UI_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(UITests ${UI_TEST_SOURCES})
target_link_libraries(UITests PRIVATE ProjectE::Engine)

e_set_target_defaults(UITests "Tests")

add_test(NAME UITests COMMAND UITests)

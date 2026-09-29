# Editor 모듈 단위 테스트 (GPU/ImGui 없이 검증 가능한 선택/Undo/복제/스냅/카메라 로직)
file(GLOB_RECURSE EDITOR_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(EditorTests ${EDITOR_TEST_SOURCES})
target_link_libraries(EditorTests PRIVATE ProjectE::Editor)

e_set_target_defaults(EditorTests "Tests")

add_test(NAME EditorTests COMMAND EditorTests)

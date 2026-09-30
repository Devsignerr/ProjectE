# AI 모듈 단위 테스트 (비헤이비어 트리 실행/중단/블랙보드, 내비메시 굽기/경로/좌표 변환)
file(GLOB_RECURSE AI_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(AITests ${AI_TEST_SOURCES})
target_link_libraries(AITests PRIVATE ProjectE::Engine)

e_set_target_defaults(AITests "Tests")

add_test(NAME AITests COMMAND AITests)

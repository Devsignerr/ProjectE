# Core 모듈 단위 테스트 (수학 라이브러리 등)
file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(CoreTests ${CORE_TEST_SOURCES})
target_link_libraries(CoreTests PRIVATE ProjectE::Engine)

e_set_target_defaults(CoreTests "Tests")

add_test(NAME CoreTests COMMAND CoreTests)

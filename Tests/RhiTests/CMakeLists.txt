# RHI 모듈 단위 테스트 (GPU 미사용: 셰이더 매니페스트/쿠킹 규칙/DXC 컴파일·캐시)
file(GLOB_RECURSE RHI_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(RhiTests ${RHI_TEST_SOURCES})
target_link_libraries(RhiTests PRIVATE ProjectE::Engine)

e_set_target_defaults(RhiTests "Tests")
e_copy_dxc_runtime(RhiTests)

add_test(NAME RhiTests COMMAND RhiTests)

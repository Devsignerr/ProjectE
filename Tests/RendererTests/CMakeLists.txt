# Renderer/Scene 모듈 단위 테스트 (GPU 없이 검증 가능한 프리미티브/카메라/씬/로더 로직)
file(GLOB_RECURSE RENDERER_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(RendererTests ${RENDERER_TEST_SOURCES})
target_link_libraries(RendererTests PRIVATE ProjectE::Renderer ProjectE::Scene)
target_compile_definitions(RendererTests PRIVATE "E_TEST_ASSET_DIR=L\"${CMAKE_SOURCE_DIR}/Sandbox/Assets\"")

e_set_target_defaults(RendererTests "Tests")

add_test(NAME RendererTests COMMAND RendererTests)

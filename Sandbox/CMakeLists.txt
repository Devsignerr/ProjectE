# Sandbox: 엔진 기능 검증용 실행 파일
file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp")

# 로그 확인을 위해 콘솔 서브시스템으로 빌드 (창 없는 실행은 이후 단계에서 도입)
add_executable(Sandbox ${SANDBOX_SOURCES})
target_link_libraries(Sandbox PRIVATE ProjectE::Core ProjectE::RHI ProjectE::Renderer)
set_target_properties(Sandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# 테스트 에셋 디렉터리 (에셋 파이프라인 도입 전까지 소스 트리에서 직접 로드)
target_compile_definitions(Sandbox PRIVATE "E_SANDBOX_ASSET_DIR=L\"${CMAKE_CURRENT_SOURCE_DIR}/Assets\"")

e_set_target_defaults(Sandbox "Sandbox")
e_copy_dxc_runtime(Sandbox)

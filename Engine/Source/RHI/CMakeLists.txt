# RHI 모듈: 렌더링 하드웨어 인터페이스 (현재 D3D12 구현만 포함)
file(GLOB_RECURSE RHI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ERHI STATIC ${RHI_SOURCES})
add_library(ProjectE::RHI ALIAS ERHI)

target_include_directories(ERHI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ERHI
    PUBLIC  ProjectE::Core
    PRIVATE d3d12 dxgi dxguid dxcompiler)

# 엔진 셰이더 소스 디렉터리 (에셋 파이프라인 도입 전까지 소스 트리에서 직접 로드)
target_compile_definitions(ERHI PRIVATE "E_ENGINE_SHADER_DIR=L\"${CMAKE_SOURCE_DIR}/Engine/Shaders\"")

e_set_target_defaults(ERHI "Engine")

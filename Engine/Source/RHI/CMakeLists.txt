# RHI 모듈: 렌더링 하드웨어 인터페이스 (현재 D3D12 구현만 포함)
file(GLOB_RECURSE RHI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ERHI STATIC ${RHI_SOURCES})
add_library(ProjectE::RHI ALIAS ERHI)

target_include_directories(ERHI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ERHI
    PUBLIC  ProjectE::Core
    PRIVATE d3d12 dxgi dxguid dxcompiler ThirdParty::nlohmann_json) # 셰이더 매니페스트(JSON)

e_set_target_defaults(ERHI "Engine")

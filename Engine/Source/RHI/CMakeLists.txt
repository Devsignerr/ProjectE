# RHI 모듈: 렌더링 하드웨어 인터페이스 (현재 D3D12 구현만 포함)
file(GLOB_RECURSE RHI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ERHI STATIC ${RHI_SOURCES})
add_library(ProjectE::RHI ALIAS ERHI)

target_include_directories(ERHI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ERHI
    PUBLIC  ProjectE::Core
    PRIVATE d3d12 dxgi dxguid dxcompiler ThirdParty::nlohmann_json ThirdParty::stb_image_write) # 셰이더 매니페스트(JSON)

# dxcompiler.dll 지연 로드 (쿠킹 셰이더만 배포한 패키지는 DXC 없이 실행). 링크 옵션은 최종 실행 파일에 적용되므로 INTERFACE
target_link_libraries(ERHI INTERFACE delayimp)
target_link_options(ERHI INTERFACE "/DELAYLOAD:dxcompiler.dll")

e_set_target_defaults(ERHI "Engine")

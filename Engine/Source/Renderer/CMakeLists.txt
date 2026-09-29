# Renderer 모듈: 카메라, 메시/프리미티브, GPU 메시, 이미지/glTF 로딩, 리소스 관리자, 머티리얼 에셋 등 RHI 위의 렌더링 계층
file(GLOB_RECURSE RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ERenderer STATIC ${RENDERER_SOURCES})
add_library(ProjectE::Renderer ALIAS ERenderer)

target_include_directories(ERenderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ERenderer
    PUBLIC  ProjectE::Core ProjectE::RHI ProjectE::Scene
    PRIVATE ThirdParty::stb_image ThirdParty::cgltf ThirdParty::nlohmann_json ThirdParty::bc7enc)

e_set_target_defaults(ERenderer "Engine")

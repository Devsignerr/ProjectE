# UI 모듈: 인게임 UI (UMG식 위젯 트리, 레이아웃, SDF 글꼴, .eui 에셋, 그리기 목록, 입력 라우팅). GPU 의존성 없음 — 그리기는 Renderer의 FUIRenderer.
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EUI ${E_ENGINE_MODULE_TYPE} ${UI_SOURCES})
add_library(ProjectE::UI ALIAS EUI)

target_include_directories(EUI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EUI
    PUBLIC  ProjectE::Core
    PRIVATE ThirdParty::nlohmann_json ThirdParty::stb_truetype)

e_set_engine_module_defaults(EUI)

# Editor 모듈: Dear ImGui 통합, 에디터 애플리케이션, 패널(계층/인스펙터/뷰포트/콘텐츠)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EEditor STATIC ${EDITOR_SOURCES})
add_library(ProjectE::Editor ALIAS EEditor)

target_include_directories(EEditor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EEditor
    PUBLIC  ProjectE::Engine
    PRIVATE ThirdParty::imgui ThirdParty::imguizmo ThirdParty::nlohmann_json ThirdParty::fontawesome comdlg32 shell32)

e_set_target_defaults(EEditor "Engine")

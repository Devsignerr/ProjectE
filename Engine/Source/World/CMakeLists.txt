# World 모듈: 게임 월드 갱신 순서(스크립트 → 게임 모듈 → 물리 → 트랜스폼 → 애니메이션 → 파티클)를 한곳에 둔다.
# 런타임, 에디터 플레이 모드, 전용 서버가 공유한다 (모듈 의존: World → Scripting/Physics/Renderer/Network → Scene → Core)
file(GLOB_RECURSE WORLD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EWorld ${E_ENGINE_MODULE_TYPE} ${WORLD_SOURCES})
add_library(ProjectE::World ALIAS EWorld)

target_include_directories(EWorld PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EWorld
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ProjectE::Scripting ProjectE::Physics ProjectE::AI ProjectE::Renderer ProjectE::Network ProjectE::Online)

e_set_engine_module_defaults(EWorld)

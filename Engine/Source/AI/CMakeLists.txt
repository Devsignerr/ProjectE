# AI 모듈: 비헤이비어 트리(BehaviorTree/) + Recast/Detour 내비게이션(Navigation/). GPU 의존성 없음.
# Recast 헤더는 이 모듈의 .cpp에서만 포함한다 (공개 헤더에 노출 금지)
file(GLOB_RECURSE AI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EAI ${E_ENGINE_MODULE_TYPE} ${AI_SOURCES})
add_library(ProjectE::AI ALIAS EAI)

target_include_directories(EAI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EAI
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ThirdParty::recast ThirdParty::nlohmann_json)

e_set_engine_module_defaults(EAI)

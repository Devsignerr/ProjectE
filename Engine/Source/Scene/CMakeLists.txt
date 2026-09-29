# Scene 모듈: ECS 기반 씬 데이터 (컴포넌트, 트랜스폼 계층, 리플렉션 등록, JSON 직렬화). GPU 의존성 없음.
file(GLOB_RECURSE SCENE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EScene ${E_ENGINE_MODULE_TYPE} ${SCENE_SOURCES})
add_library(ProjectE::Scene ALIAS EScene)

target_include_directories(EScene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EScene
    PUBLIC  ProjectE::Core
    PRIVATE ThirdParty::nlohmann_json)

e_set_engine_module_defaults(EScene)

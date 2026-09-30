# Scripting 모듈: Lua 5.4 + sol2 기반 스크립트 컴포넌트 실행 (리플렉션 자동 바인딩, 핫 리로드)
file(GLOB_RECURSE SCRIPTING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EScripting ${E_ENGINE_MODULE_TYPE} ${SCRIPTING_SOURCES})
add_library(ProjectE::Scripting ALIAS EScripting)

target_include_directories(EScripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EScripting
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ProjectE::UI ThirdParty::sol2 ThirdParty::nlohmann_json)
# sol2 템플릿이 많아 오브젝트 섹션 수 한도를 넘을 수 있다
target_compile_options(EScripting PRIVATE /bigobj)

e_set_engine_module_defaults(EScripting)

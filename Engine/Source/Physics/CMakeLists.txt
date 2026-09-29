# Physics 모듈: Jolt 기반 강체 물리 (월드 래퍼, 씬 동기화 시스템, 콜라이더/강체 컴포넌트). GPU 의존성 없음.
file(GLOB_RECURSE PHYSICS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EPhysics ${E_ENGINE_MODULE_TYPE} ${PHYSICS_SOURCES})
add_library(ProjectE::Physics ALIAS EPhysics)

target_include_directories(EPhysics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EPhysics
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ThirdParty::jolt)

e_set_engine_module_defaults(EPhysics)

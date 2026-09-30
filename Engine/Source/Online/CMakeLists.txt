# Online 모듈: 플랫폼 서비스 연동 (Steamworks). SDK가 없으면 같은 API가 "사용 불가"로 동작하는 빈 구현으로 빌드된다.
file(GLOB_RECURSE ONLINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EOnline ${E_ENGINE_MODULE_TYPE} ${ONLINE_SOURCES})
add_library(ProjectE::Online ALIAS EOnline)

target_include_directories(EOnline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EOnline PUBLIC ProjectE::Core)
if(TARGET ThirdParty::steamworks)
    target_link_libraries(EOnline PRIVATE ThirdParty::steamworks)
    target_compile_definitions(EOnline PRIVATE E_WITH_STEAM=1)
else()
    target_compile_definitions(EOnline PRIVATE E_WITH_STEAM=0)
endif()

e_set_engine_module_defaults(EOnline)

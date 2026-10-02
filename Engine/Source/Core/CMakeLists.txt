# Core 모듈: 플랫폼 추상화(창/입력), 로그, 타이머, 애플리케이션 루프
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ECore ${E_ENGINE_MODULE_TYPE} ${CORE_SOURCES})
add_library(ProjectE::Core ALIAS ECore)

# include 경로는 모듈 루트(Engine/Source) 기준: #include "Core/Log.h"
target_include_directories(ECore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ECore PRIVATE ThirdParty::nlohmann_json) # .eproject 등 JSON 파싱
# Tracy (Core/Profiling.h): E_TRACY는 모든 소비자에게 (매크로가 켜짐/빈 문장 결정)
if(E_TRACY)
    target_link_libraries(ECore PRIVATE ThirdParty::tracy)
    target_compile_definitions(ECore PUBLIC E_TRACY=1)
else()
    target_compile_definitions(ECore PUBLIC E_TRACY=0)
endif()

e_set_engine_module_defaults(ECore)

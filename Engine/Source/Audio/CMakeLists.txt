# Audio 모듈: miniaudio 기반 사운드 재생/3D 공간화, 오디오 소스 컴포넌트와 동기화 시스템. GPU 의존성 없음.
file(GLOB_RECURSE AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(EAudio ${E_ENGINE_MODULE_TYPE} ${AUDIO_SOURCES})
add_library(ProjectE::Audio ALIAS EAudio)

target_include_directories(EAudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(EAudio
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ThirdParty::miniaudio)

e_set_engine_module_defaults(EAudio)

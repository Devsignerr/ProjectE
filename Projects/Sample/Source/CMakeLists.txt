# Sample 프로젝트 게임 모듈 → Bin/SampleGame.dll (Sample.eproject "GameModule": "SampleGame")
# 엔진 DLL(ProjectE::Engine)만 링크한다. 게임 컴포넌트/시스템은 IGameModule로 엔진에 등록된다.
file(GLOB_RECURSE SAMPLE_GAME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(SampleGame SHARED ${SAMPLE_GAME_SOURCES})
target_include_directories(SampleGame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(SampleGame PRIVATE ProjectE::Engine)

e_set_target_defaults(SampleGame "Game")
set_property(GLOBAL APPEND PROPERTY E_GAME_MODULE_TARGETS SampleGame)

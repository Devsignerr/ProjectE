# FarmBie 게임 모듈 → Bin/FarmBieGame.dll (FarmBie.eproject "GameModule": "FarmBieGame")
# 매 프레임 대량 처리(농장 격자·좀비 흐름장 이동·크리스탈 감지·설치물)는 여기, 하루 진행·상점·UI 흐름은 Lua(Content/Scripts/FarmBie)
file(GLOB_RECURSE FARMBIE_GAME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(FarmBieGame SHARED ${FARMBIE_GAME_SOURCES})
target_include_directories(FarmBieGame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(FarmBieGame PRIVATE ProjectE::Engine)

e_set_target_defaults(FarmBieGame "Game")
set_property(GLOBAL APPEND PROPERTY E_GAME_MODULE_TARGETS FarmBieGame)
